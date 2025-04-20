/* copyright(c) mkfs (Maksim Yarovoy) 2025 */
/* main.cpp - base package manager for Openbase GNU/Linux */

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <curl/curl.h>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <thread>
#include <queue>
#include <future>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std;

#define COLOR_RESET    "\033[0m"
#define COLOR_RED      "\033[31m"
#define COLOR_GREEN    "\033[32m"
#define COLOR_YELLOW   "\033[33m"
#define COLOR_MAGENTA  "\033[35m"

size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* data) {
    data->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class Logger {
private:
    ofstream log_file;
    mutex log_mutex;
    bool initialized;
    string log_path = "/var/log/basepm.log";
    const size_t max_log_size = 10 * 1024 * 1024; 

    void rotateLog() {
        if (fs::exists(log_path) && fs::file_size(log_path) > max_log_size) {
            string backup = log_path + ".1";
            if (fs::exists(backup)) fs::remove(backup);
            fs::rename(log_path, backup);
        }
    }

public:
    Logger() : initialized(false) {
        try {
            rotateLog();
            log_file.open(log_path, ios::app);
            if (!log_file.is_open()) {
                throw runtime_error("Failed to open log file " + log_path);
            }
            initialized = true;
            log("Logger initialized successfully");
        } catch (const exception& e) {
            cerr << "Logger initialization failed: " << e.what() << endl;
            throw;
        }
    }

    ~Logger() {
        if (log_file.is_open()) log_file.close();
    }

    void log(const string& message, const string& level = "INFO") {
        string color;
        if (level == "INFO") color = COLOR_GREEN;
        else if (level == "WARNING") color = COLOR_YELLOW;
        else if (level == "ERROR") color = COLOR_RED;
        else if (level == "DEBUG") color = COLOR_MAGENTA;
        else color = COLOR_RESET;

        if (!initialized) {
            cerr << color << "[UNINITIALIZED] [" << level << "] " << message << COLOR_RESET << endl;
            return;
        }
        try {
            lock_guard<mutex> lock(log_mutex);
            auto now = chrono::system_clock::now();
            auto time = chrono::system_clock::to_time_t(now);
            string timestamp = ctime(&time);
            timestamp = timestamp.substr(0, timestamp.length() - 1);
            
            log_file << "[" << timestamp << "] [" << level << "] " << message << endl;
            log_file.flush();
            
            cout << color << "[" << level << "] " << message << COLOR_RESET << endl;
        } catch (const exception& e) {
            cerr << COLOR_RED << "Logging failed: " << e.what() << COLOR_RESET << endl;
        }
    }
};

struct Version {
    string value;
    Version() : value("0.0.0") {}
    Version(const string& v) : value(v) {}
    string to_string() const { return value; }

    bool operator<(const Version& other) const {
        vector<int> v1 = parseVersion(value);
        vector<int> v2 = parseVersion(other.value);
        return lexicographical_compare(v1.begin(), v1.end(), v2.begin(), v2.end());
    }

    bool operator==(const Version& other) const {
        return value == other.value;
    }

    bool operator>=(const Version& other) const {
        return !(*this < other);
    }

private:
    vector<int> parseVersion(const string& ver) const {
        vector<int> result;
        stringstream ss(ver);
        string part;
        while (getline(ss, part, '.')) {
            try {
                result.push_back(stoi(part));
            } catch (...) {
                result.push_back(0);
            }
        }
        return result;
    }
};

string operator+(const string& lhs, const Version& rhs) {
    return lhs + rhs.to_string();
}

struct Dependency {
    string name;
    Version min_version;
    Dependency(const string& dep) {
        size_t pos = dep.find(':');
        if (pos != string::npos) {
            name = dep.substr(0, pos);
            min_version = Version(dep.substr(pos + 1));
        } else {
            name = dep;
            min_version = Version("0.0.0");
        }
    }
};

class Package {
public:
    string name;
    Version version;
    vector<Dependency> dependencies;
    string source_url;
    string fetch_method;
    string architecture;
    string license;
    string install_dir;
    string checksum;

    Package() : version("0.0.0") {}
    Package(string n, string v, vector<Dependency> deps, string src_url, string fetch,
            string arch, string lic, string inst_dir, string chksum = "")
        : name(n), version(v), dependencies(deps), source_url(src_url),
          fetch_method(fetch), architecture(arch), license(lic),
          install_dir(inst_dir), checksum(chksum) {}

    bool verifyChecksum(const string& file_path) const {
        if (checksum.empty()) return true;
        return true;
    }
};

class ConfigManager {
private:
    string config_path;
    json config;
    map<string, string> build_vars;
    Logger& logger;
    const string default_repo = "https://raw.githubusercontent.com/emerge6/base-packages/main";
    const map<string, json> default_config = {
        {"cache_dir", "/var/cache/basepm"},
        {"repo", default_repo},
        {"max_cache_age_days", 7},
        {"parallel_downloads", 4},
        {"log_level", "info"},
        {"retry_attempts", 3},
        {"verbose", false},
        {"CFLAGS", "-O2 -pipe"},
        {"MAKEOPTS", "-j" + to_string(thread::hardware_concurrency())}
    };

    void validateConfig() {
        for (const auto& [key, value] : default_config) {
            if (!config.contains(key) || config[key].is_null()) {
                logger.log("Missing or null config field '" + key + "', setting default: " + value.dump(), "WARNING");
                config[key] = value;
            } else if (value.is_string() && !config[key].is_string()) {
                logger.log("Invalid type for '" + key + "', expected string, setting default: " + value.dump(), "WARNING");
                config[key] = value;
            } else if (value.is_number_integer() && !config[key].is_number_integer()) {
                logger.log("Invalid type for '" + key + "', expected integer, setting default: " + value.dump(), "WARNING");
                config[key] = value;
            } else if (value.is_boolean() && !config[key].is_boolean()) {
                logger.log("Invalid type for '" + key + "', expected boolean, setting default: " + value.dump(), "WARNING");
                config[key] = value;
            }
        }
    }

public:
    ConfigManager(Logger& logger_ref)
        : config_path("/etc/basepm/config.json"), logger(logger_ref) {
        try {
            logger.log("Initializing ConfigManager");
            if (!fs::exists(config_path)) {
                logger.log("Config file does not exist, creating default");
                config = default_config;
                saveConfig();
            } else {
                loadConfig();
            }
            validateConfig();

            try {
                build_vars["CFLAGS"] = config["CFLAGS"].get<string>();
                build_vars["MAKEOPTS"] = config["MAKEOPTS"].get<string>();
            } catch (const json::exception& e) {
                logger.log(string("Failed to initialize build_vars: ") + e.what(), "ERROR");
                throw runtime_error("Invalid build variables in config");
            }
            logger.log("ConfigManager initialized successfully");
        } catch (const exception& e) {
            logger.log(string("ConfigManager initialization failed: ") + e.what(), "ERROR");
            throw;
        }
    }

    void loadConfig() {
        ifstream file(config_path);
        if (!file.is_open()) {
            logger.log("Failed to open config file: " + config_path, "ERROR");
            throw runtime_error("Cannot open config file");
        }
        try {
            file >> config;
            logger.log("Loaded configuration from " + config_path);
        } catch (const json::exception& e) {
            file.close();
            logger.log(string("Failed to parse config file: ") + e.what(), "ERROR");
            throw runtime_error("Invalid JSON in config file");
        }
        file.close();
    }

    void saveConfig() {
        try {
            fs::create_directories(fs::path(config_path).parent_path());
            ofstream file(config_path);
            if (!file.is_open()) {
                logger.log("Failed to create config file: " + config_path, "ERROR");
                throw runtime_error("Cannot create config file");
            }
            file << config.dump(4);
            file.close();
            logger.log("Saved configuration to " + config_path);
        } catch (const exception& e) {
            logger.log(string("Failed to save config: ") + e.what(), "ERROR");
            throw;
        }
    }

    string getCacheDir() const {
        try {
            return config.at("cache_dir").get<string>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get cache_dir: ") + e.what() + ", using default: /var/cache/basepm", "WARNING");
            return "/var/cache/basepm";
        }
    }

    string getRepo() const {
        try {
            return config.at("repo").get<string>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get repo: ") + e.what() + ", using default: " + default_repo, "WARNING");
            return default_repo;
        }
    }

    int getMaxCacheAge() const {
        try {
            return config.at("max_cache_age_days").get<int>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get max_cache_age_days: ") + e.what() + ", using default: 7", "WARNING");
            return 7;
        }
    }

    int getParallelDownloads() const {
        try {
            return config.at("parallel_downloads").get<int>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get parallel_downloads: ") + e.what() + ", using default: 4", "WARNING");
            return 4;
        }
    }

    int getRetryAttempts() const {
        try {
            return config.at("retry_attempts").get<int>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get retry_attempts: ") + e.what() + ", using default: 3", "WARNING");
            return 3;
        }
    }

    bool getVerbose() const {
        try {
            return config.at("verbose").get<bool>();
        } catch (const json::exception& e) {
            logger.log(string("Failed to get verbose: ") + e.what() + ", using default: false", "WARNING");
            return false;
        }
    }

    string getBuildVar(const string& key) const {
        return build_vars.count(key) ? build_vars.at(key) : "";
    }
};

class BaseBuildParser {
public:
    Package parse(const string& filename) {
        ifstream file(filename);
        if (!file.is_open()) {
            throw runtime_error("Failed to open file: " + filename);
        }
        
        string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
        file.close();

        if (content.empty()) {
            throw runtime_error("File " + filename + " is empty");
        }

        string preview = content.length() > 500 ? content.substr(0, 500) + "..." : content;
        regex newline_regex("\n");
        preview = regex_replace(preview, newline_regex, "\\n");
        Logger().log("Parsing .basebuild file: " + filename + ", content preview: " + preview, "DEBUG");

        map<string, string> data;
        regex var_regex(R"(^\s*([A-Z_]+)\s*=\s*(\"([^\"]*?)\"|([^\s\n\"]*))\s*$)", regex::multiline);
        smatch match;
        string::const_iterator search_start(content.cbegin());

        while (regex_search(search_start, content.cend(), match, var_regex)) {
            string key = match[1].str();
            string value = match[3].str().empty() ? match[4].str() : match[3].str();
            value.erase(value.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            data[key] = value;
            search_start = match.suffix().first;
            Logger().log("Parsed key: " + key + ", value: " + value, "DEBUG");
        }

        vector<string> required_fields = {"PKG_NAME", "PKG_VERSION", "PKG_SRC_URL", "PKG_FETCH_METHOD"};
        for (const auto& field : required_fields) {
            if (data.find(field) == data.end() || data[field].empty()) {
                throw runtime_error("Missing or empty required field: " + field + " in " + filename);
            }
        }

        vector<Dependency> deps;
        if (data.count("PKG_DEPENDENCIES")) {
            stringstream ss(data["PKG_DEPENDENCIES"]);
            string dep;
            while (ss >> dep) {
                deps.emplace_back(dep);
            }
        }

        return Package(
            data["PKG_NAME"],
            data["PKG_VERSION"],
            deps,
            data["PKG_SRC_URL"],
            data["PKG_FETCH_METHOD"],
            data.count("PKG_ARCHITECTURE") ? data["PKG_ARCHITECTURE"] : "any",
            data.count("PKG_LICENSE") ? data["PKG_LICENSE"] : "Unknown",
            data.count("PKG_INSTALL_DIR") ? data["PKG_INSTALL_DIR"] : "/usr/local",
            data.count("PKG_CHECKSUM") ? data["PKG_CHECKSUM"] : ""
        );
    }
};

class BaseCore {
private:
    Logger logger;
    ConfigManager config;
    BaseBuildParser parser;
    map<string, Package> installed_packages;
    map<string, Package> package_cache;

public:
    BaseCore() : logger(), config(logger) {}

private:
    bool isCacheValid(const string& cache_file) {
        try {
            if (!fs::exists(cache_file)) return false;
            auto last_write = fs::last_write_time(cache_file);
            auto now = fs::file_time_type::clock::now();
            auto age = chrono::duration_cast<chrono::hours>(now - last_write).count() / 24;
            return age <= config.getMaxCacheAge();
        } catch (const fs::filesystem_error& e) {
            logger.log(string("Failed to check cache validity for ") + cache_file + ": " + string(e.what()), "WARNING");
            return false;
        }
    }

    string fetchBaseBuild(const string& package_name) {
        string cache_dir = config.getCacheDir();
        try {
            fs::create_directories(cache_dir);
        } catch (const fs::filesystem_error& e) {
            logger.log(string("Failed to create cache directory ") + cache_dir + ": " + string(e.what()), "ERROR");
            throw runtime_error("Cannot create cache directory");
        }

        string cache_file = cache_dir + "/" + package_name + ".basebuild";

        if (isCacheValid(cache_file)) {
            try {
                ifstream file(cache_file);
                if (!file.is_open()) {
                    logger.log("Failed to open cached .basebuild file: " + cache_file, "ERROR");
                    throw runtime_error("Cannot open cached .basebuild file");
                }
                string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
                logger.log("Using cached .basebuild for " + package_name, "DEBUG");
                return content;
            } catch (const exception& e) {
                logger.log(string("Error reading cached .basebuild for ") + package_name + ": " + string(e.what()), "WARNING");
            }
        }

        CURL* curl = curl_easy_init();
        string url = config.getRepo() + "/" + package_name + "/" + package_name + ".basebuild";
        string response;
        long http_code = 0;
        int retries = config.getRetryAttempts();

        if (!curl) {
            logger.log("Failed to initialize CURL for fetching .basebuild", "ERROR");
            throw runtime_error("CURL initialization failed");
        }

        for (int attempt = 1; attempt <= retries; ++attempt) {
            response.clear();
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Base-Package-Manager/3.3");
            CURLcode res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (res == CURLE_OK && http_code == 200) {
                logger.log("Fetched .basebuild for " + package_name + " (attempt " + to_string(attempt) + ")", "DEBUG");
                break;
            } else {
                string error = "Failed to fetch .basebuild (attempt " + to_string(attempt) + "): " +
                              (res != CURLE_OK ? string(curl_easy_strerror(res)) : "HTTP " + to_string(http_code));
                logger.log(error, "WARNING");
                if (attempt == retries) {
                    curl_easy_cleanup(curl);
                    throw runtime_error(error);
                }
                this_thread::sleep_for(chrono::seconds(1));
            }
        }
        curl_easy_cleanup(curl);

        if (response.empty()) {
            logger.log("Fetched .basebuild for " + package_name + " is empty", "ERROR");
            throw runtime_error("Empty .basebuild file fetched for " + package_name);
        }

        string preview = response.length() > 500 ? response.substr(0, 500) + "..." : response;
        regex newline_regex("\n");
        preview = regex_replace(preview, newline_regex, "\\n");
        logger.log("Fetched .basebuild content preview for " + package_name + ": " + preview, "DEBUG");

        try {
            ofstream outfile(cache_file);
            if (!outfile.is_open()) {
                logger.log("Failed to write .basebuild to cache: " + cache_file, "ERROR");
                throw runtime_error("Cannot write to cache file");
            }
            outfile << response;
            outfile.close();
            logger.log("Saved .basebuild to cache: " + cache_file, "DEBUG");
        } catch (const exception& e) {
            logger.log(string("Error writing .basebuild to cache: ") + e.what(), "ERROR");
            throw;
        }
        return response;
    }

    void downloadSource(const Package& pkg) {
        string src_dir = "/tmp/" + pkg.name + "-" + pkg.version.to_string();
        try {
            fs::create_directories(src_dir);
        } catch (const fs::filesystem_error& e) {
            logger.log(string("Failed to create source directory ") + src_dir + ": " + string(e.what()), "ERROR");
            throw runtime_error("Cannot create source directory");
        }
        
        string basebuild_file = config.getCacheDir() + "/" + pkg.name + ".basebuild";
        string cmd = "bash " + basebuild_file + " unpack";
        logger.log("Executing unpack command: " + cmd, "INFO");
        if (system(cmd.c_str()) != 0) {
            logger.log("Failed to unpack sources for " + pkg.name, "ERROR");
            throw runtime_error("Failed to unpack sources for " + pkg.name);
        }
        logger.log("Sources unpacked to " + src_dir, "INFO");
    }

    void resolveDependencies(const string& package_name, vector<string>& install_order, set<string>& visited) {
        if (visited.count(package_name)) return;
        visited.insert(package_name);

        string basebuild_content;
        try {
            basebuild_content = fetchBaseBuild(package_name);
        } catch (const exception& e) {
            logger.log(string("Failed to fetch .basebuild for ") + package_name + ": " + string(e.what()), "ERROR");
            throw;
        }

        string temp_file = config.getCacheDir() + "/" + package_name + ".basebuild";
        try {
            ofstream outfile(temp_file);
            if (!outfile.is_open()) {
                logger.log("Failed to write temporary .basebuild file: " + temp_file, "ERROR");
                throw runtime_error("Cannot write temporary .basebuild file");
            }
            outfile << basebuild_content;
            outfile.close();
        } catch (const exception& e) {
            logger.log(string("Error writing temporary .basebuild file: ") + e.what(), "ERROR");
            throw;
        }

        Package pkg;
        try {
            pkg = parser.parse(temp_file);
        } catch (const exception& e) {
            logger.log(string("Failed to parse .basebuild for ") + package_name + ": " + string(e.what()), "ERROR");
            throw;
        }
        package_cache.emplace(package_name, pkg);

        if (installed_packages.count(package_name)) {
            const Package& installed = installed_packages.at(package_name);
            bool version_satisfied = true;
            for (const auto& dep : pkg.dependencies) {
                if (installed.version >= dep.min_version) continue;
                version_satisfied = false;
                break;
            }
            if (version_satisfied) {
                logger.log("Package " + package_name + " version " + installed.version.to_string() + " satisfies requirements, skipping", "INFO");
                return;
            }
        }

        for (const auto& dep : pkg.dependencies) {
            try {
                resolveDependencies(dep.name, install_order, visited);
            } catch (const exception& e) {
                logger.log(string("Failed to resolve dependency ") + dep.name + ": " + string(e.what()), "ERROR");
                throw;
            }
        }
        install_order.push_back(package_name);
    }

public:
    void installPackage(const string& package_name, bool dry_run = false, bool verbose = false) {
        logger.log("Resolving dependencies for " + package_name, "INFO");
        vector<string> install_order;
        set<string> visited;
        try {
            resolveDependencies(package_name, install_order, visited);
        } catch (const exception& e) {
            logger.log(string("Dependency resolution failed for ") + package_name + ": " + string(e.what()), "ERROR");
            throw;
        }

        for (const string& pkg_name : install_order) {
            if (installed_packages.count(pkg_name)) {
                const Package& installed = installed_packages.at(pkg_name);
                const Package& cached = package_cache.at(pkg_name);
                bool version_satisfied = installed.version >= cached.version;
                if (version_satisfied) {
                    logger.log("Package " + pkg_name + " version " + installed.version.to_string() + " already satisfies requirements, skipping", "INFO");
                    continue;
                }
            }

            Package& pkg = package_cache.at(pkg_name);
            logger.log("Installing package: " + pkg.name + " (version: " + pkg.version.to_string() + ")", "INFO");

            if (dry_run) {
                logger.log("Dry run: Would install " + pkg.name, "INFO");
                continue;
            }

            try {
                downloadSource(pkg);
                logger.log("Building package: " + pkg.name, "INFO");
                string build_cmd = "bash " + config.getCacheDir() + "/" + pkg.name + ".basebuild compile";
                if (verbose) logger.log("Executing build command: " + build_cmd, "DEBUG");
                if (system(build_cmd.c_str()) != 0) {
                    logger.log("Build failed for " + pkg.name, "ERROR");
                    throw runtime_error("Build failed for " + pkg.name);
                }

                logger.log("Installing package: " + pkg.name, "INFO");
                string install_cmd = "bash " + config.getCacheDir() + "/" + pkg.name + ".basebuild install";
                if (verbose) logger.log("Executing install command: " + install_cmd, "DEBUG");
                if (system(install_cmd.c_str()) != 0) {
                    logger.log("Installation failed for " + pkg.name, "ERROR");
                    throw runtime_error("Installation failed for " + pkg.name);
                }

                installed_packages.emplace(pkg.name, pkg);
                savePackageDatabase();
                logger.log("Package " + pkg.name + " installed successfully", "INFO");
            } catch (const exception& e) {
                logger.log(string("Error installing ") + pkg.name + ": " + string(e.what()), "ERROR");
                throw;
            }
        }
    }

    void removePackage(const string& package_name, bool dry_run = false) {
        if (!installed_packages.count(package_name)) {
            logger.log("Package " + package_name + " is not installed", "ERROR");
            throw runtime_error("Package " + package_name + " is not installed");
        }

        Package& pkg = installed_packages.at(package_name);

        for (const auto& [other_name, other_pkg] : installed_packages) {
            if (other_name == package_name) continue;
            for (const auto& dep : other_pkg.dependencies) {
                if (dep.name == package_name) {
                    logger.log("Cannot remove " + package_name + ": required by " + other_name, "ERROR");
                    throw runtime_error("Package " + package_name + " is a dependency of " + other_name);
                }
            }
        }

        logger.log("Preparing to remove package: " + package_name, "INFO");
        if (dry_run) {
            logger.log("Dry run: Would remove " + package_name, "INFO");
            return;
        }

        string src_dir = "/tmp/" + pkg.name + "-" + pkg.version.to_string();
        if (fs::exists(src_dir)) {
            try {
                fs::remove_all(src_dir);
                logger.log("Removed source directory: " + src_dir, "INFO");
            } catch (const fs::filesystem_error& e) {
                logger.log(string("Failed to remove source directory ") + src_dir + ": " + string(e.what()), "WARNING");
            }
        }

        string binary_path = pkg.install_dir + "/bin/" + pkg.name;
        if (fs::exists(binary_path)) {
            try {
                fs::remove(binary_path);
                logger.log("Removed binary: " + binary_path, "INFO");
            } catch (const fs::filesystem_error& e) {
                logger.log(string("Failed to remove binary ") + binary_path + ": " + string(e.what()), "ERROR");
                throw runtime_error("Failed to remove binary " + binary_path);
            }
        }

        installed_packages.erase(package_name);
        savePackageDatabase();
        logger.log("Package " + package_name + " removed successfully", "INFO");
    }

    void syncCache() {
        string cache_dir = config.getCacheDir();
        logger.log("Clearing cache in " + cache_dir, "INFO");
        try {
            if (fs::exists(cache_dir)) {
                for (const auto& entry : fs::directory_iterator(cache_dir)) {
                    try {
                        fs::remove_all(entry.path());
                        logger.log("Removed cache entry: " + entry.path().string(), "DEBUG");
                    } catch (const fs::filesystem_error& e) {
                        logger.log(string("Failed to remove cache entry ") + entry.path().string() + ": " + string(e.what()), "WARNING");
                    }
                }
                logger.log("Cache cleared successfully", "INFO");
            }
            fs::create_directories(cache_dir);
            logger.log("Recreated cache directory: " + cache_dir, "INFO");
        } catch (const fs::filesystem_error& e) {
            logger.log(string("Error clearing cache: ") + string(e.what()), "ERROR");
            throw runtime_error("Error clearing cache");
        }
    }

    void listPackages() {
        logger.log("Listing installed packages", "INFO");
        if (installed_packages.empty()) {
            logger.log("No packages installed", "INFO");
            return;
        }
        for (const auto& [name, pkg] : installed_packages) {
            logger.log("Package: " + name + " (version: " + pkg.version.to_string() + ")", "INFO");
        }
    }

    void searchPackages(const string& query) {
        logger.log("Searching for packages matching: " + query, "INFO");
        logger.log("Search functionality not implemented", "WARNING");
    }

    void showPackageInfo(const string& package_name) {
        logger.log("Showing info for package: " + package_name, "INFO");
        if (installed_packages.count(package_name)) {
            const Package& pkg = installed_packages.at(package_name);
            logger.log("Name: " + pkg.name, "INFO");
            logger.log("Version: " + pkg.version.to_string(), "INFO");
            string deps;
            for (const auto& dep : pkg.dependencies) {
                deps += dep.name + ":" + dep.min_version.to_string() + " ";
            }
            logger.log("Dependencies: " + (deps.empty() ? "None" : deps), "INFO");
            logger.log("Source URL: " + pkg.source_url, "INFO");
            logger.log("Architecture: " + pkg.architecture, "INFO");
            logger.log("License: " + pkg.license, "INFO");
            logger.log("Install Dir: " + pkg.install_dir, "INFO");
        } else {
            logger.log("Package " + package_name + " is not installed", "INFO");
        }
    }

    void cleanOldVersions() {
        logger.log("Cleaning old package versions", "INFO");
        logger.log("Clean old versions functionality not implemented", "WARNING");
    }

    void savePackageDatabase() {
        json db;
        for (const auto& [name, pkg] : installed_packages) {
            vector<string> deps;
            for (const auto& dep : pkg.dependencies) {
                deps.push_back(dep.name + ":" + dep.min_version.to_string());
            }
            db[name] = {
                {"version", pkg.version.to_string()},
                {"dependencies", deps},
                {"source_url", pkg.source_url},
                {"fetch_method", pkg.fetch_method},
                {"architecture", pkg.architecture},
                {"license", pkg.license},
                {"install_dir", pkg.install_dir}
            };
        }
        try {
            fs::create_directories("/var/lib/basepm");
            ofstream file("/var/lib/basepm/installed.json");
            if (!file.is_open()) {
                logger.log("Failed to open package database file: /var/lib/basepm/installed.json", "ERROR");
                throw runtime_error("Cannot open package database file");
            }
            file << db.dump(4);
            file.close();
            logger.log("Saved package database to /var/lib/basepm/installed.json", "INFO");
        } catch (const exception& e) {
            logger.log(string("Failed to save package database: ") + string(e.what()), "ERROR");
            throw;
        }
    }

    void loadPackageDatabase() {
        if (!fs::exists("/var/lib/basepm/installed.json")) {
            logger.log("Package database does not exist, starting with empty database", "INFO");
            return;
        }

        ifstream file("/var/lib/basepm/installed.json");
        if (!file.is_open()) {
            logger.log("Failed to open package database file: /var/lib/basepm/installed.json", "WARNING");
            return;
        }

        json db;
        try {
            file >> db;
        } catch (const json::exception& e) {
            file.close();
            logger.log(string("Failed to parse package database: ") + string(e.what()), "ERROR");
            throw runtime_error("Invalid JSON in package database");
        }
        file.close();

        for (const auto& [name, data] : db.items()) {
            try {
                vector<Dependency> deps;
                if (data.contains("dependencies")) {
                    for (const auto& dep : data.at("dependencies")) {
                        deps.emplace_back(dep.get<string>());
                    }
                }

                string version = data.contains("version") ? data.at("version").get<string>() : "0.0.0";
                string source_url = data.contains("source_url") ? data.at("source_url").get<string>() : "";
                string fetch_method = data.contains("fetch_method") ? data.at("fetch_method").get<string>() : "wget";
                string architecture = data.contains("architecture") ? data.at("architecture").get<string>() : "any";
                string license = data.contains("license") ? data.at("license").get<string>() : "Unknown";
                string install_dir = data.contains("install_dir") ? data.at("install_dir").get<string>() : "/usr/local";

                installed_packages.emplace(name, Package(
                    name,
                    version,
                    deps,
                    source_url,
                    fetch_method,
                    architecture,
                    license,
                    install_dir
                ));
            } catch (const json::exception& e) {
                logger.log(string("Failed to load package ") + name + " from database: " + string(e.what()), "WARNING");
            }
        }
        logger.log("Loaded package database from /var/lib/basepm/installed.json", "INFO");
    }

    void printDepGraph(const string& package_name, int depth = 0) {
        set<string> visited;
        printDepGraphImpl(package_name, depth, visited);
    }

private:
    void printDepGraphImpl(const string& package_name, int depth, set<string>& visited) {
        if (visited.count(package_name)) return;
        visited.insert(package_name);

        string indent(depth * 2, ' ');
        logger.log(indent + package_name, "INFO");

        string basebuild_content;
        try {
            basebuild_content = fetchBaseBuild(package_name);
        } catch (const exception& e) {
            logger.log(string("Failed to fetch .basebuild for ") + package_name + ": " + string(e.what()), "ERROR");
            return;
        }

        string temp_file = config.getCacheDir() + "/" + package_name + ".basebuild";
        try {
            ofstream outfile(temp_file);
            outfile << basebuild_content;
            outfile.close();
        } catch (const exception& e) {
            logger.log(string("Failed to write temporary .basebuild file: ") + temp_file + ", error: " + string(e.what()), "ERROR");
            return;
        }

        Package pkg;
        try {
            pkg = parser.parse(temp_file);
        } catch (const exception& e) {
            logger.log(string("Failed to parse .basebuild for ") + package_name + ": " + string(e.what()), "ERROR");
            return;
        }

        for (const auto& dep : pkg.dependencies) {
            printDepGraphImpl(dep.name, depth + 1, visited);
        }
    }

    string join(const vector<string>& vec, const string& delim) {
        string result;
        for (size_t i = 0; i < vec.size(); ++i) {
            result += vec[i];
            if (i < vec.size() - 1) result += delim;
        }
        return result;
    }
};

void printHelp() {
    cout << "Base Package Manager v3.3\n"
         << "Использование: base <команда> [опции]\n\n"
         << "Команды:\n"
         << "  infuse <package>      Установить пакет и его зависимости\n"
         << "  rip <package>         Удалить указанный пакет\n"
         << "  pulse                 Очистить и синхронизировать кэш\n"
         << "  list                  Список установленных пакетов\n"
         << "  hunt <query>          Поиск пакетов по запросу\n"
         << "  lore <package>        Показать информацию о пакете\n"
         << "  clean                 Очистить старые версии пакетов\n"
         << "  web <package>         Показать граф зависимостей\n"
         << "  help, --help          Показать это сообщение\n\n"
         << "Пример:\n"
         << "  base infuse fastfetch\n"
         << "  base rip fastfetch\n"
         << "  base pulse\n"
         << "  base list\n\n"
         << "Репозиторий: https://github.com/emerge6/base-packages\n"
         << "Сообщайте об ошибках: https://t.me/+jnz-Dk-YvBQwNzli\n";
}

void printVersion() {
    cout << COLOR_MAGENTA << R"(
      _______
 ,-~~~       ~~~-,
(                 )
 \_-, , , , , ,-_/
    / / | | \ \
    | | | | | |
    | | | | | |
   / / /   \ \ \
   | | |   | | |

Openbase Package Manager
Version: 3.3
Repository: https://github.com/emerge6/base-packages
Configuration: /etc/basepm/config.json
Cache Directory: /var/cache/basepm
Database: /var/lib/basepm/installed.json
Log File: /var/log/basepm.log
License: GNU Public License v3
)" << COLOR_RESET;
}

int main(int argc, char* argv[]) {
    try {
        CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
        if (res != CURLE_OK) {
            throw runtime_error("curl_global_init failed: " + string(curl_easy_strerror(res)));
        }

        BaseCore core;
        core.loadPackageDatabase();

        if (argc < 2 || string(argv[1]) == "help" || string(argv[1]) == "--help") {
            printHelp();
            curl_global_cleanup();
            return 0;
        }

        string command = argv[1];
        if (command == "-V" || command == "--version") {
            printVersion();
            curl_global_cleanup();
            return 0;
        }

        bool dry_run = false;
        bool verbose = false;
        vector<string> args(argv + 2, argv + argc);
        if (find(args.begin(), args.end(), "--dry-run") != args.end()) {
            dry_run = true;
            args.erase(remove(args.begin(), args.end(), "--dry-run"), args.end());
        }
        if (find(args.begin(), args.end(), "--verbose") != args.end()) {
            verbose = true;
            args.erase(remove(args.begin(), args.end(), "--verbose"), args.end());
        }

        if (command == "infuse" && args.size() == 1) {
            core.installPackage(args[0], dry_run, verbose);
        } else if (command == "rip" && args.size() == 1) {
            core.removePackage(args[0], dry_run);
        } else if (command == "pulse" && args.empty()) {
            core.syncCache();
        } else if (command == "list" && args.empty()) {
            core.listPackages();
        } else if (command == "hunt" && args.size() == 1) {
            core.searchPackages(args[0]);
        } else if (command == "lore" && args.size() == 1) {
            core.showPackageInfo(args[0]);
        } else if (command == "web" && args.size() == 1) {
            core.printDepGraph(args[0]);
        } else if (command == "clean" && args.empty()) {
            core.cleanOldVersions();
        } else {
            cerr << COLOR_RED << "Unknown command or invalid arguments: " << command << COLOR_RESET << endl;
            printHelp();
            curl_global_cleanup();
            return 1;
        }
    } catch (const exception& e) {
        cerr << COLOR_RED << "Critical error: " << e.what() << COLOR_RESET << endl;
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
