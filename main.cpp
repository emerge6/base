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
    const size_t max_log_size = 10 * 1024 * 1024; // 10 MB

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
    Version(const string& v) : value(v) {}
    string to_string() const { return value; }

    bool operator<(const Version& other) const {
        return value < other.value;
    }

    bool operator==(const Version& other) const {
        return value == other.value;
    }
};

string operator+(const string& lhs, const Version& rhs) {
    return lhs + rhs.to_string();
}

class Package {
public:
    string name;
    Version version;
    vector<string> dependencies;
    string source_url;
    string fetch_method;
    string build_instructions;
    string install_instructions;
    string checksum;

    Package(string n, string v, vector<string> deps, string src_url, string fetch,
            string build, string install, string chksum = "")
        : name(n), version(v), dependencies(deps), source_url(src_url),
          fetch_method(fetch), build_instructions(build),
          install_instructions(install), checksum(chksum) {}

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

public:
    ConfigManager(Logger& logger_ref)
        : config_path("/etc/basepm/config.json"), logger(logger_ref) {
        try {
            logger.log("Initializing ConfigManager");
            if (!fs::exists(config_path)) {
                logger.log("Config file does not exist, creating default");
                config = {
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
                saveConfig();
            } else {
                loadConfig();
            }

            if (!config.contains("repo") || !config["repo"].is_string() || config["repo"].get<string>() != default_repo) {
                logger.log("Invalid or non-default repo in config, resetting to " + default_repo, "WARNING");
                config["repo"] = default_repo;
                saveConfig();
            }
            if (!config.contains("cache_dir") || !config["cache_dir"].is_string()) {
                logger.log("Invalid or missing cache_dir in config, using default", "WARNING");
                config["cache_dir"] = "/var/cache/basepm";
                saveConfig();
            }
            if (!config.contains("max_cache_age_days") || !config["max_cache_age_days"].is_number_integer()) {
                logger.log("Invalid or missing max_cache_age_days in config, using default", "WARNING");
                config["max_cache_age_days"] = 7;
                saveConfig();
            }
            if (!config.contains("parallel_downloads") || !config["parallel_downloads"].is_number_integer()) {
                logger.log("Invalid or missing parallel_downloads in config, using default", "WARNING");
                config["parallel_downloads"] = 4;
                saveConfig();
            }
            if (!config.contains("log_level") || !config["log_level"].is_string()) {
                logger.log("Invalid or missing log_level in config, using default", "WARNING");
                config["log_level"] = "info";
                saveConfig();
            }
            if (!config.contains("retry_attempts") || !config["retry_attempts"].is_number_integer()) {
                logger.log("Invalid or missing retry_attempts in config, using default", "WARNING");
                config["retry_attempts"] = 3;
                saveConfig();
            }
            if (!config.contains("verbose") || !config["verbose"].is_boolean()) {
                logger.log("Invalid or missing verbose in config, using default", "WARNING");
                config["verbose"] = false;
                saveConfig();
            }
            if (!config.contains("CFLAGS") || !config["CFLAGS"].is_string()) {
                logger.log("Invalid or missing CFLAGS in config, using default", "WARNING");
                config["CFLAGS"] = "-O2 -pipe";
                saveConfig();
            }
            if (!config.contains("MAKEOPTS") || !config["MAKEOPTS"].is_string()) {
                logger.log("Invalid or missing MAKEOPTS in config, using default", "WARNING");
                config["MAKEOPTS"] = "-j" + to_string(thread::hardware_concurrency());
                saveConfig();
            }
            build_vars["CFLAGS"] = config["CFLAGS"].get<string>();
            build_vars["MAKEOPTS"] = config["MAKEOPTS"].get<string>();
            logger.log("ConfigManager initialized successfully");
        } catch (const exception& e) {
            logger.log("ConfigManager initialization failed: " + string(e.what()), "ERROR");
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
            logger.log("Failed to parse config file: " + string(e.what()), "ERROR");
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
            logger.log("Failed to save config: " + string(e.what()), "ERROR");
            throw;
        }
    }

    string getCacheDir() const { return config["cache_dir"].get<string>(); }
    string getRepo() const { return config["repo"].get<string>(); }
    int getMaxCacheAge() const { return config["max_cache_age_days"].get<int>(); }
    int getParallelDownloads() const { return config["parallel_downloads"].get<int>(); }
    int getRetryAttempts() const { return config["retry_attempts"].get<int>(); }
    bool getVerbose() const { return config["verbose"].get<bool>(); }
    string getBuildVar(const string& key) const {
        return build_vars.count(key) ? build_vars.at(key) : "";
    }
};

class BaseBuildParser {
public:
    Package parse(const string& filename) {
        ifstream file(filename);
        if (!file.is_open()) throw runtime_error("Не удалось открыть файл: " + filename);
        
        string line;
        map<string, string> data;
        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq_pos = line.find('=');
            if (eq_pos != string::npos) {
                string key = line.substr(0, eq_pos);
                string value = line.substr(eq_pos + 1);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
                data[key] = value;
                cout << "Parsed: " << key << " = " << value << endl;
            }
        }
        file.close();

        vector<string> required_fields = {"NAME", "VERSION", "SOURCE_URL", "FETCH_METHOD", "BUILD", "INSTALL"};
        for (const auto& field : required_fields) {
            if (data.find(field) == data.end() || data[field].empty()) {
                throw runtime_error("Missing or empty required field: " + field + " in " + filename);
            }
        }

        vector<string> deps;
        if (data.count("DEPENDENCIES")) {
            stringstream ss(data["DEPENDENCIES"]);
            string dep;
            while (ss >> dep) deps.push_back(dep);
        }

        return Package(
            data["NAME"],
            data["VERSION"],
            deps,
            data["SOURCE_URL"],
            data["FETCH_METHOD"],
            data["BUILD"],
            data["INSTALL"],
            data.count("CHECKSUM") ? data["CHECKSUM"] : ""
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
        if (!fs::exists(cache_file)) return false;
        auto last_write = fs::last_write_time(cache_file);
        auto now = fs::file_time_type::clock::now();
        auto age = chrono::duration_cast<chrono::hours>(now - last_write).count() / 24;
        return age <= config.getMaxCacheAge();
    }

    string fetchBaseBuild(const string& package_name) {
        string cache_dir = config.getCacheDir();
        fs::create_directories(cache_dir);
        string cache_file = cache_dir + "/" + package_name + ".basebuild";

        if (isCacheValid(cache_file)) {
            ifstream file(cache_file);
            string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
            logger.log("Using cached .basebuild for " + package_name + ":\n" + content, "DEBUG");
            return content;
        }

        CURL* curl = curl_easy_init();
        string url = config.getRepo() + "/" + package_name + "/" + package_name + ".basebuild";
        string response;

        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Base-Package-Manager/2.0");
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                string error = "Failed to fetch .basebuild: " + string(curl_easy_strerror(res));
                logger.log(error, "ERROR");
                curl_easy_cleanup(curl);
                throw runtime_error(error);
            }
            curl_easy_cleanup(curl);
        }

        logger.log("Fetched .basebuild for " + package_name + ":\n" + response, "DEBUG");
        ofstream outfile(cache_file);
        outfile << response;
        outfile.close();
        return response;
    }

    void downloadSource(const Package& pkg) {
        if (pkg.fetch_method != "wget" && pkg.fetch_method != "curl" && pkg.fetch_method != "git") {
            logger.log("Invalid fetch method: " + pkg.fetch_method + " for " + pkg.name, "ERROR");
            throw runtime_error("Invalid fetch method: " + pkg.fetch_method + " for " + pkg.name);
        }

        string src_dir = "/tmp/" + pkg.name + "-" + pkg.version.to_string();
        fs::create_directories(src_dir);
        
        string archive_path = "/tmp/" + pkg.name + ".tar.gz";
        string cmd;
        
        logger.log("Downloading sources for " + pkg.name, "INFO");
        if (pkg.fetch_method == "wget") {
            cmd = "wget " + pkg.source_url + " -O " + archive_path;
        } else if (pkg.fetch_method == "curl") {
            cmd = "curl -L " + pkg.source_url + " -o " + archive_path;
        } else if (pkg.fetch_method == "git") {
            cmd = "git clone " + pkg.source_url + " " + src_dir;
        }

        if (system(cmd.c_str()) != 0) {
            logger.log("Failed to download sources for " + pkg.name, "ERROR");
            throw runtime_error("Failed to download sources for " + pkg.name);
        }

        if (pkg.fetch_method != "git") {
            if (!pkg.verifyChecksum(archive_path)) {
                logger.log("Checksum verification failed for " + pkg.name, "ERROR");
                throw runtime_error("Checksum verification failed for " + pkg.name);
            }
            cmd = "tar -xzf " + archive_path + " -C " + src_dir + " --strip-components=1";
            if (system(cmd.c_str()) != 0) {
                logger.log("Failed to extract sources for " + pkg.name, "ERROR");
                throw runtime_error("Failed to extract sources for " + pkg.name);
            }
        }
        logger.log("Sources downloaded and extracted to " + src_dir, "INFO");
    }

    void resolveDependencies(const string& package_name, vector<string>& install_order, set<string>& visited) {
        if (visited.count(package_name)) return;
        if (installed_packages.count(package_name)) return;

        string basebuild_content = fetchBaseBuild(package_name);
        string temp_file = config.getCacheDir() + "/" + package_name + ".basebuild";
        ofstream outfile(temp_file);
        outfile << basebuild_content;
        outfile.close();

        Package pkg = parser.parse(temp_file);
        package_cache.emplace(package_name, pkg);

        visited.insert(package_name);
        for (const string& dep : pkg.dependencies) {
            resolveDependencies(dep, install_order, visited);
        }
        install_order.push_back(package_name);
    }

public:
    void installPackage(const string& package_name, bool dry_run = false, bool verbose = false) {
        logger.log("Resolving dependencies for " + package_name, "INFO");
        vector<string> install_order;
        set<string> visited;
        resolveDependencies(package_name, install_order, visited);

        for (const string& pkg_name : install_order) {
            if (installed_packages.count(pkg_name)) {
                logger.log("Package " + pkg_name + " already installed, skipping", "INFO");
                continue;
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
                string build_cmd = "cd /tmp/" + pkg.name + "-" + pkg.version.to_string() + " && " + pkg.build_instructions;
                if (verbose) logger.log("Executing build command: " + build_cmd, "DEBUG");
                if (system(build_cmd.c_str()) != 0) {
                    logger.log("Build failed for " + pkg.name, "ERROR");
                    throw runtime_error("Build failed for " + pkg.name);
                }

                logger.log("Installing package: " + pkg.name, "INFO");
                string install_cmd = "cd /tmp/" + pkg.name + "-" + pkg.version.to_string() + " && " + pkg.install_instructions;
                if (verbose) logger.log("Executing install command: " + install_cmd, "DEBUG");
                if (system(install_cmd.c_str()) != 0) {
                    logger.log("Installation failed for " + pkg.name, "ERROR");
                    throw runtime_error("Installation failed for " + pkg.name);
                }

                installed_packages.emplace(pkg.name, pkg);
                savePackageDatabase();
                logger.log("Package " + pkg.name + " installed successfully", "INFO");
            } catch (const exception& e) {
                logger.log("Error installing " + pkg.name + ": " + e.what(), "ERROR");
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
            for (const string& dep : other_pkg.dependencies) {
                if (dep == package_name) {
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
        if (!fs::exists(src_dir)) {
            logger.log("Source directory " + src_dir + " does not exist, skipping uninstall command", "WARNING");
        } else {
            string uninstall_cmd;
            if (pkg.install_instructions.find("make install") != string::npos) {
                uninstall_cmd = pkg.install_instructions;
                size_t pos = uninstall_cmd.find("make install");
                uninstall_cmd.replace(pos, 12, "make uninstall");
            } else if (pkg.install_instructions.find("cmake --install") != string::npos) {
                logger.log("CMake-based package detected; no standard uninstall, skipping command", "WARNING");
                uninstall_cmd = "";
            } else {
                logger.log("Unknown install method for " + package_name + "; skipping uninstall command", "WARNING");
                uninstall_cmd = "";
            }

            if (!uninstall_cmd.empty()) {
                string full_cmd = "cd " + src_dir + " && " + uninstall_cmd;
                logger.log("Executing uninstall command: " + full_cmd, "INFO");
                if (system(full_cmd.c_str()) != 0) {
                    logger.log("Failed to execute uninstall command for " + package_name, "ERROR");
                    throw runtime_error("Failed to uninstall " + package_name);
                }
                logger.log("Uninstall command executed successfully for " + package_name, "INFO");
            }
        }

        if (fs::exists(src_dir)) {
            try {
                fs::remove_all(src_dir);
                logger.log("Removed source directory: " + src_dir, "INFO");
            } catch (const fs::filesystem_error& e) {
                logger.log("Failed to remove source directory " + src_dir + ": " + e.what(), "WARNING");
            }
        }

        string archive_path = "/tmp/" + pkg.name + ".tar.gz";
        if (fs::exists(archive_path)) {
            try {
                fs::remove(archive_path);
                logger.log("Removed archive: " + archive_path, "INFO");
            } catch (const fs::filesystem_error& e) {
                logger.log("Failed to remove archive " + archive_path + ": " + e.what(), "WARNING");
            }
        }

        string binary_path = "/usr/bin/" + pkg.name;
        if (fs::exists(binary_path)) {
            try {
                fs::remove(binary_path);
                logger.log("Removed binary: " + binary_path, "INFO");
            } catch (const fs::filesystem_error& e) {
                logger.log("Failed to remove binary " + binary_path + ": " + e.what(), "ERROR");
                throw runtime_error("Failed to remove binary " + binary_path);
            }
        } else {
            logger.log("Binary " + binary_path + " does not exist, skipping", "INFO");
        }

        installed_packages.erase(package_name);
        savePackageDatabase();
        logger.log("Package " + package_name + " removed successfully from database", "INFO");
    }

    void syncCache() {
        string cache_dir = config.getCacheDir();
        logger.log("Clearing cache in " + cache_dir, "INFO");
        try {
            if (fs::exists(cache_dir)) {
                for (const auto& entry : fs::directory_iterator(cache_dir)) {
                    fs::remove_all(entry.path());
                }
                logger.log("Cache cleared successfully", "INFO");
            } else {
                logger.log("Cache directory does not exist, skipping cleanup", "INFO");
            }
            fs::create_directories(cache_dir);
        } catch (const fs::filesystem_error& e) {
            logger.log("Error clearing cache: " + string(e.what()), "ERROR");
            throw runtime_error("Error clearing cache: " + string(e.what()));
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
            logger.log("Dependencies: " + (pkg.dependencies.empty() ? "None" : join(pkg.dependencies, ", ")), "INFO");
            logger.log("Source URL: " + pkg.source_url, "INFO");
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
            db[name] = {
                {"version", pkg.version.to_string()},
                {"dependencies", pkg.dependencies},
                {"source_url", pkg.source_url},
                {"fetch_method", pkg.fetch_method}
            };
        }
        ofstream file("/var/lib/basepm/installed.json");
        file << db.dump(4);
        file.close();
    }

    void loadPackageDatabase() {
        if (fs::exists("/var/lib/basepm/installed.json")) {
            ifstream file("/var/lib/basepm/installed.json");
            json db;
            file >> db;
            file.close();

            for (const auto& [name, data] : db.items()) {
                vector<string> deps;
                for (const auto& dep : data["dependencies"]) {
                    deps.push_back(dep.get<string>());
                }
                installed_packages.emplace(name, Package(
                    name,
                    data["version"].get<string>(),
                    deps,
                    data["source_url"],
                    data["fetch_method"],
                    "",
                    ""
                ));
            }
        }
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
            logger.log("Failed to fetch .basebuild for " + package_name + ": " + e.what(), "ERROR");
            return;
        }

        string temp_file = config.getCacheDir() + "/" + package_name + ".basebuild";
        ofstream outfile(temp_file);
        outfile << basebuild_content;
        outfile.close();

        Package pkg = parser.parse(temp_file);
        for (const string& dep : pkg.dependencies) {
            printDepGraphImpl(dep, depth + 1, visited);
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
         << "  web      <package>    Показать граф зависимостей\n"
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
        cerr << "Initializing CURL" << endl;
        CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
        if (res != CURLE_OK) {
            throw runtime_error("curl_global_init failed: " + string(curl_easy_strerror(res)));
        }
        cerr << "CURL initialized successfully" << endl;

        cerr << "Creating BaseCore" << endl;
        BaseCore core;
        cerr << "BaseCore created successfully" << endl;

        core.loadPackageDatabase();

        if (argc < 2 || string(argv[1]) == "help" || string(argv[1]) == "--help") {
            printHelp();
            curl_global_cleanup();
            return 0;
        }
// блять хуй пойми как это раюотает вообще
        string command = argv[1];
        if (command == "-V" || command == "--version") {
            printVersion();
            curl_global_cleanup();
            return 0;
        }

        bool dry_run = false;
        vector<string> args(argv + 2, argv + argc);
        if (find(args.begin(), args.end(), "--dry-run") != args.end()) {
            dry_run = true;
            args.erase(remove(args.begin(), args.end(), "--dry-run"), args.end());
        }

        if (command == "infuse" && args.size() == 1) {
            core.installPackage(args[0], dry_run);
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
