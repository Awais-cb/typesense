#include <cstdlib>
#include <curl/curl.h>
#include <gflags/gflags.h>
#include <dlfcn.h>
#include <brpc/controller.h>
#include <brpc/server.h>
#include <braft/raft.h>
#include <raft_server.h>
#include <fstream>
#include <execinfo.h>
#include <http_client.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>

#include "core_api.h"
#include "typesense_server_utils.h"
#include "file_utils.h"
#include "threadpool.h"
#include "jemalloc.h"

#include "stackprinter.h"

HttpServer* server;
std::atomic<bool> quit_raft_service;

extern "C" {
// weak symbol: resolved at runtime by the linker if we are using jemalloc, nullptr otherwise
#ifdef __APPLE__
    int je_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) __attribute__((weak_import));
#else
    int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) __attribute__((weak));
#endif
}

bool using_jemalloc() {
    // On OSX, jemalloc API is prefixed with "je_"
#ifdef __APPLE__
    return (je_mallctl != nullptr);
#else
    return (mallctl != nullptr);
#endif
}

void catch_interrupt(int sig) {
    LOG(INFO) << "Stopping Typesense server...";
    signal(sig, SIG_IGN);  // ignore for now as we want to shut down elegantly
    quit_raft_service = true;
}

Option<std::string> fetch_file_contents(const std::string & file_path) {
    if(!file_exists(file_path)) {
        return Option<std::string>(404, std::string("File does not exist at: ") + file_path);
    }

    std::ifstream infile(file_path);
    std::string content((std::istreambuf_iterator<char>(infile)), (std::istreambuf_iterator<char>()));
    infile.close();

    return Option<std::string>(content);
}

void init_cmdline_options(cmdline::parser & options, int argc, char **argv) {
    options.set_program_name("./typesense-server");

    options.add<std::string>("data-dir", 'd', "Directory where data will be stored.", true);
    options.add<std::string>("api-key", 'a', "API key that allows all operations.", true);
    options.add<std::string>("search-only-api-key", 's', "[DEPRECATED: use API key management end-point] API key that allows only searches.", false);

    options.add<std::string>("api-address", '\0', "Address to which Typesense API service binds.", false, "0.0.0.0");
    options.add<uint32_t>("api-port", '\0', "Port on which Typesense API service listens.", false, 8108);

    options.add<std::string>("peering-address", '\0', "Internal IP address to which Typesense peering service binds.", false, "");
    options.add<uint32_t>("peering-port", '\0', "Port on which Typesense peering service listens.", false, 8107);
    options.add<std::string>("peering-subnet", '\0', "Internal subnet that Typesense should use for peering.", false, "");
    options.add<std::string>("nodes", '\0', "Path to file containing comma separated string of all nodes in the cluster.", false);

    options.add<std::string>("ssl-certificate", 'c', "Path to the SSL certificate file.", false, "");
    options.add<std::string>("ssl-certificate-key", 'k', "Path to the SSL certificate key file.", false, "");
    options.add<uint32_t>("ssl-refresh-interval-seconds", '\0', "Frequency of automatic reloading of SSL certs from disk.", false, 8 * 60 * 60);

    options.add<bool>("enable-cors", '\0', "Enable CORS requests.", false, true);
    options.add<std::string>("cors-domains", '\0', "Comma separated list of domains that are allowed for CORS.", false, "");

    options.add<float>("max-memory-ratio", '\0', "Maximum fraction of system memory to be used.", false, 1.0f);
    options.add<int>("snapshot-interval-seconds", '\0', "Frequency of replication log snapshots.", false, 3600);
    options.add<size_t>("healthy-read-lag", '\0', "Reads are rejected if the updates lag behind this threshold.", false, 1000);
    options.add<size_t>("healthy-write-lag", '\0', "Writes are rejected if the updates lag behind this threshold.", false, 500);
    options.add<int>("log-slow-requests-time-ms", '\0', "When > 0, requests that take longer than this duration are logged.", false, -1);

    options.add<uint32_t>("num-collections-parallel-load", '\0', "Number of collections that are loaded in parallel during start up.", false, 4);
    options.add<uint32_t>("num-documents-parallel-load", '\0', "Number of documents per collection that are indexed in parallel during start up.", false, 1000);

    options.add<uint32_t>("thread-pool-size", '\0', "Number of threads used for handling concurrent requests.", false, 4);

    options.add<std::string>("log-dir", '\0', "Path to the log directory.", false, "");

    options.add<std::string>("config", '\0', "Path to the configuration file.", false, "");

    options.add<bool>("enable-access-logging", '\0', "Enable access logging.", false, false);
    options.add<int>("disk-used-max-percentage", '\0', "Reject writes when used disk space exceeds this percentage. Default: 100 (never reject).", false, 100);

    // DEPRECATED
    options.add<std::string>("listen-address", 'h', "[DEPRECATED: use `api-address`] Address to which Typesense API service binds.", false, "0.0.0.0");
    options.add<uint32_t>("listen-port", 'p', "[DEPRECATED: use `api-port`] Port on which Typesense API service listens.", false, 8108);
    options.add<std::string>("master", 'm', "[DEPRECATED: use clustering via --nodes] Master's address in http(s)://<master_address>:<master_port> format "
                                            "to start as read-only replica.", false, "");
}

int init_root_logger(Config & config, const std::string & server_version) {
    google::InitGoogleLogging("typesense");

    std::string log_dir = config.get_log_dir();

    if(log_dir.empty()) {
        // use console logger if log dir is not specified
        FLAGS_logtostderr = true;
    } else {
        if(!directory_exists(log_dir)) {
            std::cerr << "Typesense failed to start. " << "Log directory " << log_dir << " does not exist.";
            return 1;
        }

        // flush log levels above -1 immediately (INFO=0)
        FLAGS_logbuflevel = -1;

        // available only on glog master (ensures that log file name is constant)
        FLAGS_timestamp_in_logfile_name = false;

        std::string log_path = log_dir + "/" + "typesense.log";

        // will log levels INFO **and above** to the given log file
        google::SetLogDestination(google::INFO, log_path.c_str());

        // don't create symlink for INFO log
        google::SetLogSymlink(google::INFO, "");

        // don't create separate log files for each level
        google::SetLogDestination(google::WARNING, "");
        google::SetLogDestination(google::ERROR, "");
        google::SetLogDestination(google::FATAL, "");

        std::cout << "Log directory is configured as: " << log_dir << std::endl;
    }

    return 0;
}

Option<std::string> fetch_nodes_config(const std::string& path_to_nodes) {
    std::string nodes_config;

    if(!path_to_nodes.empty()) {
        const Option<std::string> & nodes_op = fetch_file_contents(path_to_nodes);

        if(!nodes_op.ok()) {
            return Option<std::string>(500, "Error reading file containing nodes configuration: " + nodes_op.error());
        } else {
            nodes_config = nodes_op.get();
            if(nodes_config.empty()) {
                return Option<std::string>(500, "File containing nodes configuration is empty.");
            } else {
                nodes_config = nodes_op.get();
            }
        }
    }

    return Option<std::string>(nodes_config);
}

bool is_private_ip(uint32_t ip) {
    uint8_t b1, b2;
    b1 = (uint8_t) (ip >> 24);
    b2 = (uint8_t) ((ip >> 16) & 0x0ff);

    // 10.x.y.z
    if (b1 == 10) {
        return true;
    }

    // 172.16.0.0 - 172.31.255.255
    if ((b1 == 172) && (b2 >= 16) && (b2 <= 31)) {
        return true;
    }

    // 192.168.0.0 - 192.168.255.255
    if ((b1 == 192) && (b2 == 168)) {
        return true;
    }

    return false;
}

const char* get_internal_ip(const std::string& subnet_cidr) {
    struct ifaddrs *ifap;
    getifaddrs(&ifap);

    uint32_t netip = 0, netbits = 0;

    if(!subnet_cidr.empty()) {
        std::vector<std::string> subnet_parts;
        StringUtils::split(subnet_cidr, subnet_parts, "/");
        if(subnet_parts.size() == 2) {
            butil::ip_t subnet_addr;
            auto res = butil::str2ip(subnet_parts[0].c_str(), &subnet_addr);
            if(res == 0) {
                netip = subnet_addr.s_addr;
                if(StringUtils::is_uint32_t(subnet_parts[1])) {
                    netbits = std::stoll(subnet_parts[1]);
                }
            }
        }
    }

    if(netip != 0 && netbits != 0) {
        LOG(INFO) << "Using subnet ip: " << netip << ", bits: " << netbits;
    }

    for(auto ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family==AF_INET) {
            auto sa = (struct sockaddr_in *) ifa->ifa_addr;
            auto ipaddr = sa->sin_addr.s_addr;
            if(is_private_ip(ntohl(ipaddr))) {
                if(netip != 0 && netbits != 0) {
                    unsigned int mask = 0xFFFFFFFF << (32 - netbits);
                    if((ntohl(netip) & mask) != (ntohl(ipaddr) & mask)) {
                        LOG(INFO) << "Skipping interface " << ifa->ifa_name << " as it does not match peering subnet.";
                        continue;
                    }
                }
                char *ip = inet_ntoa(sa->sin_addr);
                freeifaddrs(ifap);
                return ip;
            }
        }
    }

    LOG(WARNING) << "Found no matching interfaces, using loopback address as internal IP.";

    freeifaddrs(ifap);
    return "127.0.0.1";
}

int start_raft_server(ReplicationState& replication_state, const std::string& state_dir, const std::string& path_to_nodes,
                      const std::string& peering_address, uint32_t peering_port, const std::string& peering_subnet,
                      uint32_t api_port, int snapshot_interval_seconds) {

    if(path_to_nodes.empty()) {
        LOG(INFO) << "Since no --nodes argument is provided, starting a single node Typesense cluster.";
    }

    const Option<std::string>& nodes_config_op = fetch_nodes_config(path_to_nodes);

    if(!nodes_config_op.ok()) {
        LOG(ERROR) << nodes_config_op.error();
        exit(-1);
    }

    butil::ip_t peering_ip;
    int ip_conv_status = 0;

    if(!peering_address.empty()) {
        ip_conv_status = butil::str2ip(peering_address.c_str(), &peering_ip);
    } else {
        const char* internal_ip = get_internal_ip(peering_subnet);
        ip_conv_status = butil::str2ip(internal_ip, &peering_ip);
    }

    if(ip_conv_status != 0) {
        LOG(ERROR) << "Failed to parse peering address `" << peering_address << "`";
        return -1;
    }

    butil::EndPoint peering_endpoint(peering_ip, peering_port);

    // start peering server
    brpc::Server raft_server;

    if (braft::add_service(&raft_server, peering_endpoint) != 0) {
        LOG(ERROR) << "Failed to add peering service";
        exit(-1);
    }

    if (raft_server.Start(peering_endpoint, nullptr) != 0) {
        LOG(ERROR) << "Failed to start peering service";
        exit(-1);
    }

    // NOTE: braft uses `election_timeout_ms / 2` as the brpc channel `timeout_ms` configuration,
    // which in turn is the upper bound for brpc `connect_timeout_ms` value.
    // Reference: https://github.com/apache/incubator-brpc/blob/122770d/docs/en/client.md#timeout
    size_t election_timeout_ms = 5000;

    if (replication_state.start(peering_endpoint, api_port, election_timeout_ms, snapshot_interval_seconds, state_dir,
                                nodes_config_op.get(), quit_raft_service) != 0) {
        LOG(ERROR) << "Failed to start peering state";
        exit(-1);
    }

    LOG(INFO) << "Typesense peering service is running on " << raft_server.listen_address();
    LOG(INFO) << "Snapshot interval configured as: " << snapshot_interval_seconds << "s";

    // Wait until 'CTRL-C' is pressed. then Stop() and Join() the service
    size_t raft_counter = 0;
    while (!brpc::IsAskedToQuit() && !quit_raft_service.load()) {
        // post-increment to ensure that we refresh right away on a fresh boot
        if(raft_counter % 10 == 0) {
            // reset peer configuration periodically to identify change in cluster membership
            const Option<std::string> & refreshed_nodes_op = fetch_nodes_config(path_to_nodes);
            if(!refreshed_nodes_op.ok()) {
                LOG(WARNING) << "Error while refreshing peer configuration: " << refreshed_nodes_op.error();
                continue;
            }
            const std::string& nodes_config = ReplicationState::to_nodes_config(peering_endpoint, api_port,
                    refreshed_nodes_op.get());
            replication_state.refresh_nodes(nodes_config);
        }

        if(raft_counter % 3 == 0) {
            // update node catch up status periodically, take care of logging too verbosely
            bool log_msg = (raft_counter % 9 == 0);
            replication_state.refresh_catchup_status(log_msg);
        }

        if(raft_counter % 60 == 0) {
            replication_state.do_snapshot();
        }

        raft_counter++;
        sleep(1);
    }

    LOG(INFO) << "Typesense peering service is going to quit.";

    // Stop application before server
    replication_state.shutdown();

    LOG(INFO) << "raft_server.stop()";
    raft_server.Stop(0);

    LOG(INFO) << "raft_server.join()";
    raft_server.Join();

    LOG(INFO) << "Typesense peering service has quit.";

    return 0;
}

int run_server(const Config & config, const std::string & version, void (*master_server_routes)()) {
    LOG(INFO) << "Starting Typesense " << version << std::flush;

    if(using_jemalloc()) {
        LOG(INFO) << "Typesense is using jemalloc.";

        // Due to time based decay depending on application not being idle-ish, set `background_thread`
        // to help with releasing memory back to the OS and improve tail latency.
        // See: https://github.com/jemalloc/jemalloc/issues/1398
        bool background_thread = true;
#ifdef __APPLE__
        je_mallctl("background_thread", nullptr, nullptr, &background_thread, sizeof(bool));
#elif __linux__
        mallctl("background_thread", nullptr, nullptr, &background_thread, sizeof(bool));
#endif
    } else {
        LOG(WARNING) << "Typesense is NOT using jemalloc.";
    }

    quit_raft_service = false;

    if(!directory_exists(config.get_data_dir())) {
        LOG(ERROR) << "Typesense failed to start. " << "Data directory " << config.get_data_dir()
                 << " does not exist.";
        return 1;
    }

    if(!config.get_master().empty()) {
        LOG(ERROR) << "The --master option has been deprecated. Please use clustering for high availability. "
                   << "Look for the --nodes configuration in the documentation.";
        return 1;
    }

    if(!config.get_search_only_api_key().empty()) {
        LOG(WARNING) << "!!!! WARNING !!!!";
        LOG(WARNING) << "The --search-only-api-key has been deprecated. "
                        "The API key generation end-point should be used for generating keys with specific ACL.";
    }

    std::string data_dir = config.get_data_dir();
    std::string db_dir = config.get_data_dir() + "/db";
    std::string state_dir = config.get_data_dir() + "/state";
    std::string meta_dir = config.get_data_dir() + "/meta";

    size_t thread_pool_size = config.get_thread_pool_size();

    const size_t proc_count = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t num_threads = thread_pool_size == 0 ? (proc_count * 8) : thread_pool_size;

    size_t num_collections_parallel_load = config.get_num_collections_parallel_load();
    num_collections_parallel_load = (num_collections_parallel_load == 0) ?
                                    (proc_count * 4) : num_collections_parallel_load;

    LOG(INFO) << "Thread pool size: " << num_threads;
    ThreadPool app_thread_pool(num_threads);
    ThreadPool server_thread_pool(num_threads);

    // primary DB used for storing the documents: we will not use WAL since Raft provides that
    Store store(db_dir);

    // meta DB for storing house keeping things
    Store meta_store(meta_dir, 24*60*60, 1024, false);

    curl_global_init(CURL_GLOBAL_SSL);
    HttpClient & httpClient = HttpClient::get_instance();
    httpClient.init(config.get_api_key());

    server = new HttpServer(
        version,
        config.get_api_address(),
        config.get_api_port(),
        config.get_ssl_cert(),
        config.get_ssl_cert_key(),
        config.get_ssl_refresh_interval_seconds() * 1000,
        config.get_enable_cors(),
        config.get_cors_domains(),
        &server_thread_pool
    );

    server->set_auth_handler(handle_authentication);

    server->on(HttpServer::STREAM_RESPONSE_MESSAGE, HttpServer::on_stream_response_message);
    server->on(HttpServer::REQUEST_PROCEED_MESSAGE, HttpServer::on_request_proceed_message);
    server->on(HttpServer::DEFER_PROCESSING_MESSAGE, HttpServer::on_deferred_processing_message);

    bool ssl_enabled = (!config.get_ssl_cert().empty() && !config.get_ssl_cert_key().empty());

    BatchedIndexer* batch_indexer = new BatchedIndexer(server, &store, &meta_store, num_threads);

    CollectionManager & collectionManager = CollectionManager::get_instance();
    collectionManager.init(&store, &app_thread_pool, config.get_max_memory_ratio(),
                           config.get_api_key(), quit_raft_service, batch_indexer);

    // first we start the peering service

    ReplicationState replication_state(server, batch_indexer, &store,
                                       &app_thread_pool, server->get_message_dispatcher(),
                                       ssl_enabled,
                                       &config,
                                       num_collections_parallel_load,
                                       config.get_num_documents_parallel_load());

    std::thread raft_thread([&replication_state, &config, &state_dir,
                             &app_thread_pool, &server_thread_pool, batch_indexer]() {

        std::thread batch_indexing_thread([batch_indexer]() {
            batch_indexer->run();
        });

        std::string path_to_nodes = config.get_nodes();
        start_raft_server(replication_state, state_dir, path_to_nodes,
                          config.get_peering_address(),
                          config.get_peering_port(),
                          config.get_peering_subnet(),
                          config.get_api_port(),
                          config.get_snapshot_interval_seconds());

        LOG(INFO) << "Shutting down batch indexer...";
        batch_indexer->stop();

        LOG(INFO) << "Waiting for batch indexing thread to be done...";
        batch_indexing_thread.join();

        LOG(INFO) << "Shutting down server_thread_pool";

        server_thread_pool.shutdown();

        LOG(INFO) << "Shutting down app_thread_pool.";

        app_thread_pool.shutdown();

        server->stop();
    });

    LOG(INFO) << "Starting API service...";

    master_server_routes();
    int ret_code = server->run(&replication_state);

    // we are out of the event loop here

    LOG(INFO) << "Typesense API service has quit.";
    quit_raft_service = true;  // we set this once again in case API thread crashes instead of a signal
    raft_thread.join();

    LOG(INFO) << "Deleting batch indexer";

    delete batch_indexer;

    LOG(INFO) << "CURL clean up";

    curl_global_cleanup();

    LOG(INFO) << "Deleting server";

    delete server;

    LOG(INFO) << "CollectionManager dispose, this might take some time...";

    CollectionManager::get_instance().dispose();

    LOG(INFO) << "Bye.";

    return ret_code;
}
