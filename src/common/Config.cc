#include "common/Config.hpp"

#include <stdlib.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include "common/Logger.hpp"

static std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

bool IniFile::load(const std::string &path, std::string &err)
{
    std::ifstream f(path);
    if (!f)
    {
        err = "cannot open config file: " + path;
        return false;
    }
    std::string line, section;
    int lineno = 0;
    while (std::getline(f, line))
    {
        lineno++;
        // 去掉行内注释(; 或 #), 但保留 URL 中的 '#'(前面必须是空白)
        for (size_t i = 0; i < line.size(); i++)
        {
            if ((line[i] == ';' || line[i] == '#') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
            {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            if (line.back() != ']')
            {
                err = "bad section at line " + std::to_string(lineno);
                return false;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            data_[section];
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            err = "expect key=value at line " + std::to_string(lineno);
            return false;
        }
        data_[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return true;
}

std::vector<std::string> IniFile::sectionsWithPrefix(const std::string &prefix) const
{
    // 按数字后缀排序: source0, source1, ..., source10
    std::vector<std::pair<int, std::string>> found;
    for (auto &kv : data_)
    {
        const std::string &name = kv.first;
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string suffix = name.substr(prefix.size());
        if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string::npos)
            continue;
        found.emplace_back(atoi(suffix.c_str()), name);
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto &p : found)
        out.push_back(p.second);
    return out;
}

std::string IniFile::getString(const std::string &sec, const std::string &key, const std::string &def) const
{
    auto s = data_.find(sec);
    if (s == data_.end())
        return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

int IniFile::getInt(const std::string &sec, const std::string &key, int def) const
{
    std::string v = getString(sec, key);
    return v.empty() ? def : atoi(v.c_str());
}

double IniFile::getDouble(const std::string &sec, const std::string &key, double def) const
{
    std::string v = getString(sec, key);
    return v.empty() ? def : atof(v.c_str());
}

bool IniFile::getBool(const std::string &sec, const std::string &key, bool def) const
{
    std::string v = lower(getString(sec, key));
    if (v.empty())
        return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool AppConfig::loadFromFile(const std::string &path, AppConfig &cfg, std::string &err)
{
    IniFile ini;
    if (!ini.load(path, err))
        return false;
    cfg = AppConfig();

    GeneralConfig &g = cfg.general;
    g.log_level = ini.getString("general", "log_level", g.log_level);
    g.perf_interval = ini.getInt("general", "perf_interval", g.perf_interval);
    g.display = ini.getBool("general", "display", g.display);
    g.use_rga = ini.getBool("general", "use_rga", g.use_rga);

    for (auto &sec : ini.sectionsWithPrefix("source"))
    {
        SourceConfig s;
        s.name = ini.getString(sec, "name", sec);
        s.url = ini.getString(sec, "url");
        s.enable = ini.getBool(sec, "enable", s.enable);
        s.loop = ini.getBool(sec, "loop", s.loop);
        s.realtime = ini.getBool(sec, "realtime", s.realtime);
        s.rtsp_transport = ini.getString(sec, "rtsp_transport", s.rtsp_transport);
        s.timeout_ms = ini.getInt(sec, "timeout_ms", s.timeout_ms);
        s.reconnect_max_ms = ini.getInt(sec, "reconnect_max_ms", s.reconnect_max_ms);
        if (s.enable)
            cfg.sources.push_back(s);
    }

    DecoderConfig &d = cfg.decoder;
    d.backend = lower(ini.getString("decoder", "backend", d.backend));
    d.mpp_split_parse = ini.getInt("decoder", "mpp_split_parse", d.mpp_split_parse);
    d.packet_queue = ini.getInt("decoder", "packet_queue", d.packet_queue);

    for (auto &sec : ini.sectionsWithPrefix("model"))
    {
        if (!ini.getBool(sec, "enable", true))
            continue;
        ModelConfig m;
        m.name = ini.getString(sec, "name", sec);
        m.path = ini.getString(sec, "path");
        m.labels = ini.getString(sec, "labels", m.labels);
        m.core = lower(ini.getString(sec, "core", m.core));
        m.instances = ini.getInt(sec, "instances", m.instances);
        m.conf_thresh = (float)ini.getDouble(sec, "conf_thresh", m.conf_thresh);
        m.nms_thresh = (float)ini.getDouble(sec, "nms_thresh", m.nms_thresh);
        m.weight = (float)ini.getDouble(sec, "weight", m.weight);
        m.letterbox = ini.getBool(sec, "letterbox", m.letterbox);
        cfg.models.push_back(m);
    }

    InferConfig &inf = cfg.infer;
    inf.infer_interval = ini.getInt("infer", "infer_interval", inf.infer_interval);
    inf.target_fps = ini.getDouble("infer", "target_fps", inf.target_fps);
    inf.reuse_max_ms = ini.getInt("infer", "reuse_max_ms", inf.reuse_max_ms);
    inf.draw_model_boxes = ini.getBool("infer", "draw_model_boxes", inf.draw_model_boxes);
    inf.threads = ini.getInt("infer", "threads", inf.threads);

    FusionConfig &f = cfg.fusion;
    f.method = lower(ini.getString("fusion", "method", f.method));
    f.iou_thresh = (float)ini.getDouble("fusion", "iou_thresh", f.iou_thresh);
    f.nms_thresh = (float)ini.getDouble("fusion", "nms_thresh", f.nms_thresh);
    f.score_thresh = (float)ini.getDouble("fusion", "score_thresh", f.score_thresh);
    f.min_votes = ini.getInt("fusion", "min_votes", f.min_votes);
    f.class_agnostic = ini.getBool("fusion", "class_agnostic", f.class_agnostic);

    MosaicConfig &mo = cfg.mosaic;
    mo.width = ini.getInt("mosaic", "width", mo.width);
    mo.height = ini.getInt("mosaic", "height", mo.height);
    mo.fps = ini.getDouble("mosaic", "fps", mo.fps);
    mo.cols = ini.getInt("mosaic", "cols", mo.cols);
    mo.show_label = ini.getBool("mosaic", "show_label", mo.show_label);

    for (auto &sec : ini.sectionsWithPrefix("push"))
    {
        PushConfig p;
        p.name = sec;
        p.enable = ini.getBool(sec, "enable", p.enable);
        p.type = lower(ini.getString(sec, "type", p.type));
        p.url = ini.getString(sec, "url");
        p.source = lower(ini.getString(sec, "source", p.source));
        p.width = ini.getInt(sec, "width", p.width);
        p.height = ini.getInt(sec, "height", p.height);
        p.fps = ini.getDouble(sec, "fps", p.fps);
        p.bitrate_kbps = ini.getInt(sec, "bitrate_kbps", p.bitrate_kbps);
        p.gop = ini.getInt(sec, "gop", p.gop);
        p.encoder = lower(ini.getString(sec, "encoder", p.encoder));
        p.overlay = ini.getBool(sec, "overlay", p.overlay);
        p.rtsp_transport = ini.getString(sec, "rtsp_transport", p.rtsp_transport);
        p.timeout_ms = ini.getInt(sec, "timeout_ms", p.timeout_ms);
        p.reconnect_max_ms = ini.getInt(sec, "reconnect_max_ms", p.reconnect_max_ms);

        Gb28181Config &gb = p.gb;
        gb.sip_server_ip = ini.getString(sec, "sip_server_ip", gb.sip_server_ip);
        gb.sip_server_port = ini.getInt(sec, "sip_server_port", gb.sip_server_port);
        gb.sip_server_id = ini.getString(sec, "sip_server_id", gb.sip_server_id);
        gb.sip_domain = ini.getString(sec, "sip_domain", gb.sip_domain);
        gb.device_id = ini.getString(sec, "device_id", gb.device_id);
        gb.channel_id = ini.getString(sec, "channel_id", gb.channel_id);
        gb.device_name = ini.getString(sec, "device_name", gb.device_name);
        gb.password = ini.getString(sec, "password", gb.password);
        gb.local_ip = ini.getString(sec, "local_ip", gb.local_ip);
        gb.local_sip_port = ini.getInt(sec, "local_sip_port", gb.local_sip_port);
        gb.media_port = ini.getInt(sec, "media_port", gb.media_port);
        gb.expires = ini.getInt(sec, "expires", gb.expires);
        gb.keepalive = ini.getInt(sec, "keepalive", gb.keepalive);
        gb.keepalive_max_miss = ini.getInt(sec, "keepalive_max_miss", gb.keepalive_max_miss);
        gb.on_duplicate_invite = lower(ini.getString(sec, "on_duplicate_invite", gb.on_duplicate_invite));
        if (p.enable)
            cfg.pushes.push_back(p);
    }
    return cfg.validate(err);
}

AppConfig AppConfig::makeDefault(const std::string &model, const std::vector<std::string> &sources)
{
    AppConfig cfg;
    for (size_t i = 0; i < sources.size(); i++)
    {
        SourceConfig s;
        s.name = "ch" + std::to_string(i);
        s.url = sources[i];
        cfg.sources.push_back(s);
    }
    ModelConfig m;
    m.name = "model0";
    m.path = model;
    cfg.models.push_back(m);
    return cfg;
}

bool AppConfig::validate(std::string &err) const
{
    if (sources.empty())
    {
        err = "no enabled [sourceN] configured";
        return false;
    }
    for (auto &s : sources)
    {
        if (s.url.empty())
        {
            err = "source '" + s.name + "' has empty url";
            return false;
        }
    }
    if (models.empty())
    {
        err = "no enabled [modelN] configured";
        return false;
    }
    if (models.size() > 32)
    {
        err = "at most 32 models are supported";
        return false;
    }
    for (auto &m : models)
    {
        if (m.path.empty() || m.instances <= 0)
        {
            err = "model '" + m.name + "' needs path and instances>0";
            return false;
        }
    }
    static const std::set<std::string> methods = {"nms", "weighted", "confidence"};
    if (!methods.count(fusion.method))
    {
        err = "fusion.method must be nms|weighted|confidence";
        return false;
    }
    if (infer.infer_interval < 1)
    {
        err = "infer.infer_interval must be >= 1";
        return false;
    }
    static const std::set<std::string> types = {"rtmp", "rtsp", "gb28181"};
    std::set<std::string> urls;
    for (auto &p : pushes)
    {
        if (!types.count(p.type))
        {
            err = p.name + ": type must be rtmp|rtsp|gb28181";
            return false;
        }
        if (p.type != "gb28181")
        {
            if (p.url.empty())
            {
                err = p.name + ": url is required";
                return false;
            }
            // 同一地址重复推流会被服务器拒绝(重复会话), 启动前就拦截
            if (!urls.insert(p.url).second)
            {
                err = p.name + ": duplicated push url " + p.url;
                return false;
            }
        }
        else if (p.gb.sip_server_ip.empty())
        {
            err = p.name + ": sip_server_ip is required for gb28181";
            return false;
        }
        if (p.source != "mosaic")
        {
            char *end = nullptr;
            long idx = strtol(p.source.c_str(), &end, 10);
            if (end == p.source.c_str() || *end != '\0' || idx < 0 || idx >= (long)sources.size())
            {
                err = p.name + ": source must be 'mosaic' or a channel index < " + std::to_string(sources.size());
                return false;
            }
        }
    }
    return true;
}

void AppConfig::dump() const
{
    LOGI("config: %zu source(s), %zu model(s), %zu push(es), display=%d rga=%d", sources.size(), models.size(),
         pushes.size(), general.display, general.use_rga);
    for (size_t i = 0; i < sources.size(); i++)
        LOGI("  source[%zu] %s: %s", i, sources[i].name.c_str(), sources[i].url.c_str());
    for (size_t i = 0; i < models.size(); i++)
        LOGI("  model[%zu] %s: %s instances=%d core=%s weight=%.2f", i, models[i].name.c_str(),
             models[i].path.c_str(), models[i].instances, models[i].core.c_str(), models[i].weight);
    LOGI("  infer: interval=%d target_fps=%.1f; fusion: %s iou=%.2f min_votes=%d", infer.infer_interval,
         infer.target_fps, fusion.method.c_str(), fusion.iou_thresh, fusion.min_votes);
    for (auto &p : pushes)
        LOGI("  %s: %s %s source=%s", p.name.c_str(), p.type.c_str(),
             p.type == "gb28181" ? p.gb.sip_server_ip.c_str() : p.url.c_str(), p.source.c_str());
}
