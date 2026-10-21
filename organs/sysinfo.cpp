// organ: sysinfo — live host telemetry snapshot
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <fstream>
#include <sys/statvfs.h>
#include <dirent.h>

namespace {

std::string jstr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { /* skip control */ }
                else out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string readTrim(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

unsigned long long readCounter(const std::string& path) {
    std::ifstream f(path);
    unsigned long long v = 0;
    if (f) f >> v;
    return v;
}

// ---- cpu: aggregate + per-core times from /proc/stat ----

struct CpuTimes {
    unsigned long long idle = 0;
    unsigned long long total = 0;
    bool ok = false;
};

// reads the aggregate "cpu" line plus every "cpuN" line, in file order
void readAllCpuTimes(CpuTimes& aggregate, std::vector<std::string>& coreNames,
                      std::vector<CpuTimes>& coreTimes) {
    aggregate = CpuTimes{};
    coreNames.clear();
    coreTimes.clear();
    std::ifstream f("/proc/stat");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        std::istringstream iss(line);
        std::string label;
        iss >> label;
        unsigned long long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
        iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        CpuTimes t;
        t.idle = idle + iowait;
        t.total = user + nice + system + idle + iowait + irq + softirq + steal;
        t.ok = true;
        if (label == "cpu") {
            aggregate = t;
        } else {
            coreNames.push_back(label);
            coreTimes.push_back(t);
        }
    }
}

double pctFromDelta(const CpuTimes& a, const CpuTimes& b) {
    if (!a.ok || !b.ok || b.total <= a.total) return 0.0;
    unsigned long long dTotal = b.total - a.total;
    unsigned long long dIdle = (b.idle >= a.idle) ? (b.idle - a.idle) : 0;
    if (dTotal == 0) return 0.0;
    double p = 100.0 * (1.0 - (double)dIdle / (double)dTotal);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

// ---- mem: /proc/meminfo ----

struct MemInfo {
    unsigned long long usedBytes = 0;
    unsigned long long totalBytes = 0;
    bool ok = false;
};

MemInfo readMemInfo() {
    MemInfo m;
    std::ifstream f("/proc/meminfo");
    if (!f) return m;
    std::map<std::string, double> vals;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        double val;
        if (!(iss >> key)) continue;
        if (!key.empty() && key.back() == ':') key.pop_back();
        if (!(iss >> val)) continue;
        vals[key] = val;
    }
    if (vals.count("MemTotal")) {
        double totalKb = vals["MemTotal"];
        double avail = vals.count("MemAvailable") ? vals["MemAvailable"] : 0.0;
        m.totalBytes = (unsigned long long)(totalKb * 1024.0);
        m.usedBytes = (unsigned long long)((totalKb - avail) * 1024.0);
        m.ok = true;
    }
    return m;
}

// ---- vols: /proc/mounts + statvfs ----

struct VolInfo {
    std::string device;
    std::string mount;
    unsigned long long used = 0;
    unsigned long long total = 0;
    bool root = false;
};

std::vector<VolInfo> readVols() {
    std::vector<VolInfo> vols;
    std::ifstream f("/proc/mounts");
    if (!f) return vols;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string device, mount, fstype;
        if (!(iss >> device >> mount >> fstype)) continue;
        if (device.rfind("/dev/", 0) != 0) continue; // only real block devices
        struct statvfs sv;
        if (statvfs(mount.c_str(), &sv) != 0) continue;
        if (sv.f_blocks == 0) continue;
        VolInfo v;
        v.device = device;
        v.mount = mount;
        unsigned long long bs = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        v.total = (unsigned long long)sv.f_blocks * bs;
        v.used = (unsigned long long)(sv.f_blocks - sv.f_bfree) * bs;
        v.root = (mount == "/");
        vols.push_back(v);
    }
    // de-dup by mount point, keep first (last mount wins in /proc/mounts order,
    // but first-seen is fine for a snapshot)
    std::vector<VolInfo> uniq;
    for (auto& v : vols) {
        bool seen = false;
        for (auto& u : uniq) if (u.mount == v.mount) { seen = true; break; }
        if (!seen) uniq.push_back(v);
    }
    return uniq;
}

// ---- nets: /sys/class/net ----

struct NetInfo {
    std::string name;
    std::string address;
    std::string state;
    unsigned long long rx = 0;
    unsigned long long tx = 0;
};

std::vector<NetInfo> readNets() {
    std::vector<NetInfo> nets;
    DIR* d = opendir("/sys/class/net");
    if (!d) return nets;
    struct dirent* ent;
    std::vector<std::string> names;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& name : names) {
        std::string base = "/sys/class/net/" + name;
        NetInfo n;
        n.name = name;
        n.address = readTrim(base + "/address");
        n.state = readTrim(base + "/operstate");
        if (n.state.empty()) n.state = "unknown";
        n.rx = readCounter(base + "/statistics/rx_bytes");
        n.tx = readCounter(base + "/statistics/tx_bytes");
        nets.push_back(n);
    }
    return nets;
}

double readUptimeMs() {
    std::ifstream f("/proc/uptime");
    double up = 0;
    if (f) f >> up;
    return up * 1000.0;
}

std::string buildSnapshot() {
    // two live samples of /proc/stat (aggregate + per-core), a short interval apart,
    // as the only real source for cpu.percent / per-core util
    CpuTimes aggA, aggB;
    std::vector<std::string> coreNamesA, coreNamesB;
    std::vector<CpuTimes> coreTimesA, coreTimesB;

    readAllCpuTimes(aggA, coreNamesA, coreTimesA);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    readAllCpuTimes(aggB, coreNamesB, coreTimesB);

    double cpuPercent = pctFromDelta(aggA, aggB);

    std::vector<double> coreUtil;
    for (size_t i = 0; i < coreNamesB.size(); ++i) {
        double u = 0.0;
        for (size_t j = 0; j < coreNamesA.size(); ++j) {
            if (coreNamesA[j] == coreNamesB[i]) { u = pctFromDelta(coreTimesA[j], coreTimesB[i]); break; }
        }
        coreUtil.push_back(u);
    }

    MemInfo mem = readMemInfo();
    std::vector<VolInfo> vols = readVols();
    std::vector<NetInfo> nets = readNets();
    double uptimeMs = readUptimeMs();

    std::ostringstream j;
    j << "{";
    j << "\"cpu\":{\"percent\":" << cpuPercent << "},";
    j << "\"mem\":{\"used\":" << mem.usedBytes << ",\"total\":" << mem.totalBytes << "},";

    j << "\"vols\":[";
    for (size_t i = 0; i < vols.size(); ++i) {
        if (i) j << ",";
        j << "{\"device\":" << jstr(vols[i].device)
          << ",\"mount\":" << jstr(vols[i].mount)
          << ",\"used\":" << vols[i].used
          << ",\"total\":" << vols[i].total
          << ",\"root\":" << (vols[i].root ? "true" : "false") << "}";
    }
    j << "],";

    j << "\"nets\":[";
    for (size_t i = 0; i < nets.size(); ++i) {
        if (i) j << ",";
        j << "{\"name\":" << jstr(nets[i].name)
          << ",\"address\":" << jstr(nets[i].address)
          << ",\"state\":" << jstr(nets[i].state)
          << ",\"rx\":" << nets[i].rx
          << ",\"tx\":" << nets[i].tx << "}";
    }
    j << "],";

    j << "\"devs\":[";
    for (size_t i = 0; i < coreNamesB.size(); ++i) {
        if (i) j << ",";
        j << "{\"name\":" << jstr(coreNamesB[i])
          << ",\"type\":\"cpu\""
          << ",\"util\":" << coreUtil[i] << "}";
    }
    j << "],";

    j << "\"uptime_ms\":" << (unsigned long long)uptimeMs;
    j << "}";
    return j.str();
}

} // namespace

extern "C" const char* aios_organ(const char* request) {
    static std::string out;
    try {
        std::string req = request ? trim(request) : "";
        if (req.empty()) {
            out = "error: empty request";
            return out.c_str();
        }
        if (req == "describe") {
            out = "sysinfo: on request \"snapshot\" returns one live JSON telemetry object "
                  "{cpu:{percent}, mem:{used,total}, vols:[{device,mount,used,total,root}], "
                  "nets:[{name,address,state,rx,tx}], devs:[{name,type,util}], uptime_ms} "
                  "read directly from /proc and /sys — no other requests are handled.";
            return out.c_str();
        }
        if (req == "snapshot") {
            out = buildSnapshot();
            return out.c_str();
        }
        out = "error: unknown request '" + req + "', try 'snapshot' or 'describe'";
        return out.c_str();
    } catch (const std::exception& e) {
        out = std::string("error: ") + e.what();
        return out.c_str();
    } catch (...) {
        out = "error: unknown failure";
        return out.c_str();
    }
}
