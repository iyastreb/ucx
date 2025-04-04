/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2012. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "test_helpers.h"

#include <ucs/async/async.h>
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/string.h>
#include <ucs/config/global_opts.h>
#include <ucs/config/parser.h>

#include <set>

extern "C" {
// On some platforms users have to declare environ explicitly
extern char** environ;
}

namespace ucs {

typedef std::pair<std::string, ::testing::TimeInMillis> test_result_t;

const double test_timeout_in_sec = 180.;

double watchdog_timeout = 900.; // 15 minutes

static test_watchdog_t watchdog;

std::set< const ::testing::TestInfo*> skipped_tests;

void *watchdog_func(void *arg)
{
    int ret = 0;
    double now;
    struct timespec timeout;

    pthread_mutex_lock(&watchdog.mutex);

    // sync with the watched thread
    pthread_barrier_wait(&watchdog.barrier);

    do {
        now = ucs_get_accurate_time();
        ucs_sec_to_timespec(now + watchdog.timeout, &timeout);

        ret = pthread_cond_timedwait(&watchdog.cv, &watchdog.mutex, &timeout);
        if (!ret) {
            pthread_barrier_wait(&watchdog.barrier);
        } else {
            // something wrong happened - handle it
            ADD_FAILURE() << strerror(ret) << " - abort testing";
            if (ret == ETIMEDOUT) {
                pthread_kill(watchdog.watched_thread, watchdog.kill_signal);
            } else {
                abort();
            }
        }

        switch (watchdog.state) {
        case WATCHDOG_TEST:
            watchdog.kill_signal = SIGTERM;
            // reset when the test completed
            watchdog.state = WATCHDOG_DEFAULT_SET;
            break;
        case WATCHDOG_RUN:
            // yawn - nothing to do
            break;
        case WATCHDOG_STOP:
            // force the end of the loop
            ret = 1;
            break;
        case WATCHDOG_TIMEOUT_SET:
            // reset when the test completed
            watchdog.state = WATCHDOG_DEFAULT_SET;
            break;
        case WATCHDOG_DEFAULT_SET:
            watchdog.timeout     = watchdog_timeout;
            watchdog.state       = WATCHDOG_RUN;
            watchdog.kill_signal = SIGABRT;
            break;
        }
    } while (!ret);

    pthread_mutex_unlock(&watchdog.mutex);

    return NULL;
}

void watchdog_signal(bool barrier)
{
    pthread_mutex_lock(&watchdog.mutex);
    pthread_cond_signal(&watchdog.cv);
    pthread_mutex_unlock(&watchdog.mutex);

    if (barrier) {
        pthread_barrier_wait(&watchdog.barrier);
    }
}

void watchdog_set(test_watchdog_state_t new_state, double new_timeout)
{
    pthread_mutex_lock(&watchdog.mutex);
    // change timeout value
    watchdog.timeout = new_timeout;
    watchdog.state   = new_state;
    // apply new value for timeout
    watchdog_signal(0);
    pthread_mutex_unlock(&watchdog.mutex);

    pthread_barrier_wait(&watchdog.barrier);
}

void watchdog_set(test_watchdog_state_t new_state)
{
    watchdog_set(new_state, watchdog_timeout);
}

void watchdog_set(double new_timeout)
{
    watchdog_set(WATCHDOG_TIMEOUT_SET, new_timeout);
}

#define WATCHDOG_DEFINE_GETTER(_what, _what_type) \
    _what_type UCS_PP_TOKENPASTE(watchdog_get_, _what)() \
    { \
        _what_type value; \
        \
        pthread_mutex_lock(&watchdog.mutex); \
        value = watchdog._what; \
        pthread_mutex_unlock(&watchdog.mutex); \
        \
        return value; \
    }

WATCHDOG_DEFINE_GETTER(timeout, double)
WATCHDOG_DEFINE_GETTER(state, test_watchdog_state_t)
WATCHDOG_DEFINE_GETTER(kill_signal, int)

int watchdog_start()
{
    pthread_mutexattr_t mutex_attr;
    ucs_status_t status;
    int ret;

    ret = pthread_mutexattr_init(&mutex_attr);
    if (ret != 0) {
        return -1;
    }
    // create reentrant mutex
    ret = pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    if (ret != 0) {
        goto err_destroy_mutex_attr;
    }

    ret = pthread_mutex_init(&watchdog.mutex, &mutex_attr);
    if (ret != 0) {
        goto err_destroy_mutex_attr;
    }

    ret = pthread_cond_init(&watchdog.cv, NULL);
    if (ret != 0) {
        goto err_destroy_mutex;
    }

    // 2 - watched thread + watchdog
    ret = pthread_barrier_init(&watchdog.barrier, NULL, 2);
    if (ret != 0) {
        goto err_destroy_cond;
    }

    pthread_mutex_lock(&watchdog.mutex);
    watchdog.state          = WATCHDOG_RUN;
    watchdog.timeout        = watchdog_timeout;
    watchdog.kill_signal    = SIGABRT;
    watchdog.watched_thread = pthread_self();
    pthread_mutex_unlock(&watchdog.mutex);

    status = ucs_pthread_create(&watchdog.thread, watchdog_func, NULL,
                                "watchdog");
    if (status != UCS_OK) {
        goto err_destroy_barrier;
    }

    pthread_mutexattr_destroy(&mutex_attr);

    // sync with the watchdog thread
    pthread_barrier_wait(&watchdog.barrier);

    // test signaling
    watchdog_signal();

    return 0;

err_destroy_barrier:
    pthread_barrier_destroy(&watchdog.barrier);
err_destroy_cond:
    pthread_cond_destroy(&watchdog.cv);
err_destroy_mutex:
    pthread_mutex_destroy(&watchdog.mutex);
err_destroy_mutex_attr:
    pthread_mutexattr_destroy(&mutex_attr);
    return -1;
}

void watchdog_stop()
{
    void *ret_val;

    pthread_mutex_lock(&watchdog.mutex);
    watchdog.state = WATCHDOG_STOP;
    watchdog_signal(0);
    pthread_mutex_unlock(&watchdog.mutex);

    pthread_barrier_wait(&watchdog.barrier);
    pthread_join(watchdog.thread, &ret_val);

    pthread_barrier_destroy(&watchdog.barrier);
    pthread_cond_destroy(&watchdog.cv);
    pthread_mutex_destroy(&watchdog.mutex);
}

static bool test_results_cmp(const test_result_t &a, const test_result_t &b)
{
    return a.second > b.second;
}

void analyze_test_results()
{
    // GTEST_REPORT_LONGEST_TESTS=100 will report TOP-100 longest tests
    /* coverity[tainted_data_return] */
    char *env_p = getenv("GTEST_REPORT_LONGEST_TESTS");
    if (env_p == NULL) {
        return;
    }

    size_t total_skipped_cnt                   = skipped_tests.size();
    ::testing::TimeInMillis total_skipped_time = 0;
    size_t max_name_size                       = 0;
    std::set< const ::testing::TestInfo*>::iterator skipped_it;
    int top_n;

    if (!strcmp(env_p, "*")) {
        top_n = std::numeric_limits<int>::max();
    } else {
        top_n = atoi(env_p);
        if (!top_n) {
            return;
        }
    }

    ::testing::UnitTest *unit_test = ::testing::UnitTest::GetInstance();
    std::vector<test_result_t> test_results;

    if (unit_test == NULL) {
        ADD_FAILURE() << "Unable to get the Unit Test instance";
        return;
    }

    for (int i = 0; i < unit_test->total_test_case_count(); i++) {
        const ::testing::TestCase *test_case = unit_test->GetTestCase(i);
        if (test_case == NULL) {
            ADD_FAILURE() << "Unable to get the Test Case instance with index "
                          << i;
            return;
        }

        for (int i = 0; i < test_case->total_test_count(); i++) {
            const ::testing::TestInfo *test = test_case->GetTestInfo(i);
            if (test == NULL) {
                ADD_FAILURE() << "Unable to get the Test Info instance with index "
                              << i;
                return;
            }

            if (test->should_run()) {
                const ::testing::TestResult *result = test->result();
                std::string test_name               = test->test_case_name();

                test_name += ".";
                test_name += test->name();

                test_results.push_back(std::make_pair(test_name,
                                                      result->elapsed_time()));

                max_name_size = std::max(test_name.size(), max_name_size);

                skipped_it = skipped_tests.find(test);
                if (skipped_it != skipped_tests.end()) {
                    total_skipped_time += result->elapsed_time();
                    skipped_tests.erase(skipped_it);
                }
            }
        }
    }

    std::sort(test_results.begin(), test_results.end(), test_results_cmp);

    top_n = std::min((int)test_results.size(), top_n);
    if (!top_n) {
        return;
    }

    // Print TOP-<N> slowest tests
    int max_index_size = ucs::to_string(top_n).size();
    std::cout << std::endl << "TOP-" << top_n << " longest tests:" << std::endl;

    for (int i = 0; i < top_n; i++) {
        std::cout << std::setw(max_index_size - ucs::to_string(i + 1).size() + 1)
                  << (i + 1) << ". " << test_results[i].first
                  << std::setw(max_name_size - test_results[i].first.size() + 3)
                  << " - " << test_results[i].second << " ms" << std::endl;
    }

    // Print skipped tests statistics
    std::cout << std::endl << "Skipped tests: count - "
              << total_skipped_cnt << ", time - "
              << total_skipped_time << " ms" << std::endl;
}

int test_time_multiplier()
{
    int factor = 1;
    if (RUNNING_ON_VALGRIND) {
        factor *= 20;
    }
#if _BullseyeCoverage
    factor *= 10;
#endif
#ifdef __SANITIZE_ADDRESS__
    factor *= 20;
#endif

    return factor;
}

ucs_time_t get_deadline(double timeout_in_sec)
{
    return ucs_get_time() +
           ucs_time_from_sec(ucs_min(watchdog_get_timeout() * 0.75,
                                     timeout_in_sec * test_time_multiplier()));
}

int max_tcp_connections()
{
    static int max_conn = 0;

    if (max_conn == 0) {
        /* assume no more than 100 fd-s are already used and consider
         * that each side of the connection could create 2 socket fds
         * (1 - from ucp_ep_create() API function, 2 - from accepting
         * the remote connection), i.e. 4 socket fds per connection  */
        max_conn = std::min((ucs_sys_max_open_files() - 100) / 4,
                            65535 - 1024/* limit on number of ports */);
    }

    return max_conn;
}

void fill_random(void *data, size_t size)
{
    if (ucs::test_time_multiplier() > 1) {
        memset(data, 0, size);
        return;
    }

    uint64_t seed = rand();
    for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
        ((uint64_t*)data)[i] = seed;
        seed = seed * 10 + 17;
    }
    size_t remainder = size % sizeof(uint64_t);
    memset((char*)data + size - remainder, 0xab, remainder);
}

scoped_setenv::scoped_setenv(const char *name, const char *value) : m_name(name) {
    if (getenv(name)) {
        m_old_value = getenv(name);
    }
    setenv(m_name.c_str(), value, 1);
}

scoped_setenv::~scoped_setenv() {
    if (!m_old_value.empty()) {
        setenv(m_name.c_str(), m_old_value.c_str(), 1);
    } else {
        unsetenv(m_name.c_str());
    }
}

ucx_env_cleanup::ucx_env_cleanup() {
    const size_t prefix_len = strlen(UCS_DEFAULT_ENV_PREFIX);
    char **envp;

    for (envp = environ; *envp != NULL; ++envp) {
        std::string env_var = *envp;

        if ((env_var.find("=") != std::string::npos) &&
            (env_var.find(UCS_DEFAULT_ENV_PREFIX, 0, prefix_len) != std::string::npos)) {
            ucx_env_storage.push_back(env_var);
        }
    }

    for (size_t i = 0; i < ucx_env_storage.size(); i++) {
        std::string var_name =
            ucx_env_storage[i].substr(0, ucx_env_storage[i].find("="));

        unsetenv(var_name.c_str());
    }
}

ucx_env_cleanup::~ucx_env_cleanup() {
    while (!ucx_env_storage.empty()) {
        std::string var_name =
            ucx_env_storage.back().substr(0, ucx_env_storage.back().find("="));
        std::string var_value =
            ucx_env_storage.back().substr(ucx_env_storage.back().find("=") + 1);

        setenv(var_name.c_str(), var_value.c_str(), 1);
        ucx_env_storage.pop_back();
    }
}

void safe_sleep(double sec) {
    ucs_time_t current_time = ucs_get_time();
    ucs_time_t end_time = current_time + ucs_time_from_sec(sec);

    while (current_time < end_time) {
        usleep((long)ucs_time_to_usec(end_time - current_time));
        current_time = ucs_get_time();
    }
}

void safe_usleep(double usec) {
    safe_sleep(usec * 1e-6);
}

bool is_inet_addr(const struct sockaddr* ifa_addr) {
    if (ifa_addr == NULL) {
        return false;
    }

    if (ifa_addr->sa_family == AF_INET6) {
        /* Skip IPv6 link-local and loopback address, that could not be used for
           connection establishment */
        auto saddr6 = (const struct sockaddr_in6*)ifa_addr;
        return !IN6_IS_ADDR_LOOPBACK(&saddr6->sin6_addr) &&
               !IN6_IS_ADDR_LINKLOCAL(&saddr6->sin6_addr);
    } else {
        return ifa_addr->sa_family == AF_INET;
    }
}

static bool netif_has_sysfs_file(const char *ifa_name, const char *file_name)
{
    std::string path(PATH_MAX, '\0');

    ucs_snprintf_safe(&path[0], path.size(), "/sys/class/net/%s/%s", ifa_name,
                      file_name);

    struct stat st;
    return stat(path.c_str(), &st) >= 0;
}

bool is_interface_usable(struct ifaddrs *ifa)
{
    return ucs_netif_flags_is_active(ifa->ifa_flags) &&
           ucs::is_inet_addr(ifa->ifa_addr) &&
           !netif_has_sysfs_file(ifa->ifa_name, "bridge") &&
           !netif_has_sysfs_file(ifa->ifa_name, "brport") &&
           !netif_has_sysfs_file(ifa->ifa_name, "wireless");
}


ssize_t get_proc_self_status_field(const std::string &parameter)
{
    const std::string path("/proc/self/status");
    std::ifstream proc_stats(path);
    std::string line, name;
    ssize_t value;

    while (std::getline(proc_stats, line)) {
        if (!(std::istringstream(line) >> name >> value)) {
            continue;
        }
        if (name == (parameter + ":")) {
            return value;
        }
    }

    UCS_TEST_MESSAGE << path << " does not contain " << parameter << " value";
    return -1;
}

std::vector<std::string> read_dir(const std::string &path)
{
    std::vector<std::string> result;
    struct dirent *entry;
    DIR *dir;

    dir = opendir(path.c_str());
    if (dir == NULL) {
        goto out_close;
    }

    for (entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (entry->d_name[0] != '.') {
            result.push_back(entry->d_name);
        }
    }

out_close:
    closedir(dir);
    return result;
}

static std::map<std::string, std::string> get_all_rdmacm_net_devices()
{
    static const std::string sysfs_ib_dir  = "/sys/class/infiniband";
    static const std::string sysfs_net_dir = "/sys/class/net";
    static const std::string ndevs_fmt     = sysfs_ib_dir +
                                             "/%s/ports/%d/gid_attrs/ndevs/0";
    static const std::string node_guid_fmt = sysfs_ib_dir + "/%s/node_guid";
    std::map<std::string, std::string> devices;
    char dev_name[32];
    char guid_buf[32];
    ssize_t nread;
    int port_num;

    if (ucs::is_aws()) {
        return devices;
    }

    std::vector<std::string> ndevs = read_dir(sysfs_net_dir);

    /* Enumerate IPoIB and RoCE devices which have direct mapping to an RDMA
     * device.
     */
    for (size_t i = 0; i < ndevs.size(); ++i) {
        std::string infiniband_dir          = sysfs_net_dir + "/" + ndevs[i] +
                                              "/device/infiniband";
        std::vector<std::string> ib_devices = read_dir(infiniband_dir);

        if (!ib_devices.empty()) {
            std::string ib_device = ib_devices.front();
            if (ib_device.rfind("smi", 0) == 0) {
                /* Skip SMI device */
                continue;
            }

            std::string ports_dir = infiniband_dir + "/" + ib_device +
                                    "/ports";
            std::string ib_port   = read_dir(ports_dir).front();

            devices.emplace(ndevs[i], ib_device + ":" + ib_port);
        }
    }

    /* Enumerate all RoCE devices, including bonding (RoCE LAG). Some devices
     * can be found again, but std::set will eliminate the duplicates.
      */
    std::vector<std::string> rdma_devs = read_dir(sysfs_ib_dir);
    for (size_t i = 0; i < rdma_devs.size(); ++i) {
        const char *ndev_name = rdma_devs[i].c_str();

        for (port_num = 1; port_num <= 2; ++port_num) {
            nread = ucs_read_file_str(dev_name, sizeof(dev_name), 1,
                                      ndevs_fmt.c_str(), ndev_name, port_num);
            if (nread <= 0) {
                continue;
            }

            memset(guid_buf, 0, sizeof(guid_buf));
            nread = ucs_read_file_str(guid_buf, sizeof(guid_buf), 1,
                                      node_guid_fmt.c_str(), ndev_name);
            if (nread <= 0) {
                continue;
            }

            /* use the device if node_guid != 0 */
            if (strstr(guid_buf, "0000:0000:0000:0000") == NULL) {
                devices.emplace(ucs_strtrim(dev_name),
                                std::string(ndev_name) + ":" +
                                ucs::to_string(port_num));
            }
        }
    }

    return devices;
}

std::string get_rdmacm_netdev(const char *ifa_name)
{
    static bool initialized = false;
    static std::map<std::string, std::string> devices;

    if (!initialized) {
        devices     = get_all_rdmacm_net_devices();
        initialized = true;
    }

    auto dev = devices.find(ifa_name);
    return (dev != devices.end()) ? dev->second : "";
}

bool is_rdmacm_netdev(const char *ifa_name)
{
    return !get_rdmacm_netdev(ifa_name).empty();
}

bool is_aws()
{
    static bool result, initialized = false;

    if (!initialized) {
        const char *str = getenv("CLOUD_TYPE");
        result          = (str != NULL) && !strcmp(str, "aws");
        initialized     = true;
    }

    return result;
}

uint16_t get_port() {
    int sock_fd, ret;
    ucs_status_t status;
    struct sockaddr_in addr_in, ret_addr;
    socklen_t len = sizeof(ret_addr);
    uint16_t port;

    status = ucs_socket_create(AF_INET, SOCK_STREAM, 0, &sock_fd);
    EXPECT_EQ(status, UCS_OK);

    memset(&addr_in, 0, sizeof(struct sockaddr_in));
    addr_in.sin_family      = AF_INET;
    addr_in.sin_addr.s_addr = INADDR_ANY;

    do {
        addr_in.sin_port        = htons(0);
        /* Ports below 1024 are considered "privileged" (can be used only by
         * user root). Ports above and including 1024 can be used by anyone */
        ret = bind(sock_fd, (struct sockaddr*)&addr_in,
                   sizeof(struct sockaddr_in));
    } while (ret);

    ret = getsockname(sock_fd, (struct sockaddr*)&ret_addr, &len);
    EXPECT_EQ(ret, 0);
    EXPECT_LT(1023, ntohs(ret_addr.sin_port)) ;

    port = ntohs(ret_addr.sin_port);
    close(sock_fd);
    return port;
}

mmap_fixed_address::mmap_fixed_address(size_t length) : m_length(length) {
    m_ptr = mmap(NULL, m_length, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m_ptr == MAP_FAILED) {
        UCS_TEST_ABORT("mmap failed to allocate memory region");
    }
}

mmap_fixed_address::~mmap_fixed_address() {
    if (m_ptr != NULL) {
        munmap(m_ptr, m_length);
    }
}

std::string compact_string(const std::string &str, size_t length)
{
    if (str.length() <= length * 2) {
        return str;
    }

    return str.substr(0, length) + "..." + str.substr(str.length() - length);
}

std::string exit_status_info(int exit_status)
{
    std::stringstream ss;

    if (WIFEXITED(exit_status)) {
        ss << ", exited with status " << WEXITSTATUS(exit_status);
    }
    if (WIFSIGNALED(exit_status)) {
        ss << ", signaled with status " << WTERMSIG(exit_status);
    }
    if (WIFSTOPPED(exit_status)) {
        ss << ", stopped with status " << WSTOPSIG(exit_status);
    }

    return ss.str().substr(2, std::string::npos);
}

size_t limit_buffer_size(size_t size)
{
    return std::min(size, std::min(ucs_get_phys_mem_size() / 16,
                                   ucs_get_memfree_size() / 4));
}

sock_addr_storage::sock_addr_storage() :
        m_size(0), m_is_valid(false), m_is_rdmacm_netdev(false)
{
    memset(&m_storage, 0, sizeof(m_storage));
}

sock_addr_storage::sock_addr_storage(const ucs_sock_addr_t &ucs_sock_addr,
                                     bool is_rdmacm_netdev,
                                     std::string netdev_name,
                                     std::string rdmacm_netdev_name)
{
    if (sizeof(m_storage) < ucs_sock_addr.addrlen) {
        memset(&m_storage, 0, sizeof(m_storage));
        m_size             = 0;
        m_is_valid         = false;
        m_is_rdmacm_netdev = false;
        m_netdev_name      = "";
    } else {
        set_sock_addr(*ucs_sock_addr.addr, ucs_sock_addr.addrlen,
                      is_rdmacm_netdev, netdev_name);
    }
}

void sock_addr_storage::set_sock_addr(const struct sockaddr &addr,
                                      const size_t size, bool is_rdmacm_netdev,
                                      std::string netdev_name)
{
    ASSERT_GE(sizeof(m_storage), size);
    ASSERT_TRUE(ucs::is_inet_addr(&addr));
    memcpy(&m_storage, &addr, size);
    m_size               = size;
    m_is_valid           = true;
    m_is_rdmacm_netdev   = is_rdmacm_netdev;
    m_netdev_name        = netdev_name;
}

void sock_addr_storage::reset_to_any() {
    ASSERT_TRUE(m_is_valid);

    if (get_sock_addr_ptr()->sa_family == AF_INET) {
        struct sockaddr_in sin = {0};

        sin.sin_family      = AF_INET;
        sin.sin_addr.s_addr = INADDR_ANY;
        sin.sin_port        = get_port();

        set_sock_addr(*(struct sockaddr*)&sin, sizeof(sin));
    } else {
        ASSERT_EQ(get_sock_addr_ptr()->sa_family, AF_INET6);
        struct sockaddr_in6 sin = {0};

        sin.sin6_family = AF_INET6;
        sin.sin6_addr   = in6addr_any;
        sin.sin6_port   = get_port();

        set_sock_addr(*(struct sockaddr*)&sin, sizeof(sin));
    }
}

bool
sock_addr_storage::operator==(const struct sockaddr_storage &sockaddr) const {
    ucs_status_t status;
    int result = ucs_sockaddr_cmp(get_sock_addr_ptr(),
                                  (const struct sockaddr*)&sockaddr, &status);
    ASSERT_UCS_OK(status);
    return result == 0;
}

void sock_addr_storage::set_port(uint16_t port) {
    if (get_sock_addr_ptr()->sa_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&m_storage;
        addr_in->sin_port = htons(port);
    } else {
        ASSERT_TRUE(get_sock_addr_ptr()->sa_family == AF_INET6);
        struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&m_storage;
        addr_in->sin6_port = htons(port);
    }
}

uint16_t sock_addr_storage::get_port() const {
    if (get_sock_addr_ptr()->sa_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&m_storage;
        return ntohs(addr_in->sin_port);
    } else {
        EXPECT_TRUE(get_sock_addr_ptr()->sa_family == AF_INET6);

        struct sockaddr_in6 *addr_in = (struct sockaddr_in6 *)&m_storage;
        return ntohs(addr_in->sin6_port);
    }
}

bool sock_addr_storage::is_rdmacm_netdev() const
{
    return m_is_rdmacm_netdev;
}

std::string sock_addr_storage::netdev_name() const
{
    return m_netdev_name;
}

size_t sock_addr_storage::get_addr_size() const {
    return m_size;
}

ucs_sock_addr_t sock_addr_storage::to_ucs_sock_addr() const {
    ucs_sock_addr_t addr;

    addr.addr    = get_sock_addr_ptr();
    addr.addrlen = m_size;
    return addr;
}

std::string sock_addr_storage::to_str() const {
    char str[UCS_SOCKADDR_STRING_LEN];
    return ucs_sockaddr_str(get_sock_addr_ptr(), str, sizeof(str));
}

std::string sock_addr_storage::to_ip_str() const {
    char str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;

    status = ucs_sockaddr_get_ipstr(get_sock_addr_ptr(), str, sizeof(str));
    ASSERT_UCS_OK(status);
    return str;
}

const struct sockaddr* sock_addr_storage::get_sock_addr_ptr() const {
    return m_is_valid ? (struct sockaddr *)(&m_storage) : NULL;
}

const void* sock_addr_storage::get_sock_addr_in_buf() const {
    const struct sockaddr* saddr = get_sock_addr_ptr();

    ucs_assert_always(saddr != NULL);
    ucs_assert_always((saddr->sa_family == AF_INET) ||
                      (saddr->sa_family == AF_INET6));

    return (saddr->sa_family == AF_INET) ?
           (const void*)&((struct sockaddr_in*)saddr)->sin_addr :
           (const void*)&((struct sockaddr_in6*)saddr)->sin6_addr;
}

std::ostream& operator<<(std::ostream& os, const sock_addr_storage& sa_storage)
{
    return os << ucs::sockaddr_to_str(sa_storage.get_sock_addr_ptr());
}

auto_buffer::auto_buffer(size_t size) : m_buf(size) {
}

void* auto_buffer::operator*() {
    return as<void>();
};

scoped_log_level::scoped_log_level(ucs_log_level_t level)
    : m_prev_level(ucs_global_opts.log_component.log_level)
{
    ucs_global_opts.log_component.log_level = level;
}

scoped_log_level::~scoped_log_level()
{
    ucs_global_opts.log_component.log_level = m_prev_level;
}

namespace detail {

message_stream::message_stream(const std::string& title) {
    static const char PADDING[] = "          ";
    static const size_t WIDTH = strlen(PADDING);

    msg <<  "[";
    msg.write(PADDING, ucs_max(WIDTH - 1, title.length()) - title.length());
    msg << title << " ] ";
}

message_stream::~message_stream() {
    msg << std::endl;
    std::cout << msg.str() << std::flush;
}

} // detail

scoped_async_lock::scoped_async_lock(ucs_async_context_t &async) :
    m_async(async)
{
    UCS_ASYNC_BLOCK(&m_async);
}

scoped_async_lock::~scoped_async_lock()
{
    UCS_ASYNC_UNBLOCK(&m_async);
}

scoped_mutex_lock::scoped_mutex_lock(pthread_mutex_t &mutex) : m_mutex(mutex)
{
    pthread_mutex_lock(&m_mutex);
}

scoped_mutex_lock::~scoped_mutex_lock()
{
    pthread_mutex_unlock(&m_mutex);
}

const std::vector<std::vector<ucs_memory_type_t> >& supported_mem_type_pairs()
{
    static std::vector<std::vector<ucs_memory_type_t> > result;

    if (result.empty()) {
        result = ucs::make_pairs(mem_buffer::supported_mem_types());
    }

    return result;
}

void skip_on_address_sanitizer()
{
#ifdef __SANITIZE_ADDRESS__
    UCS_TEST_SKIP_R("Address sanitizer");
#endif
}

} // ucs
