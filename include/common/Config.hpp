#ifndef COMMON_CONFIG_HPP
#define COMMON_CONFIG_HPP

#include <map>
#include <string>
#include <vector>

// ---------------- INI 解析 ----------------
class IniFile
{
public:
    bool load(const std::string &path, std::string &err);
    bool hasSection(const std::string &sec) const { return data_.count(sec) != 0; }
    std::vector<std::string> sectionsWithPrefix(const std::string &prefix) const;

    std::string getString(const std::string &sec, const std::string &key, const std::string &def = "") const;
    int getInt(const std::string &sec, const std::string &key, int def) const;
    double getDouble(const std::string &sec, const std::string &key, double def) const;
    bool getBool(const std::string &sec, const std::string &key, bool def) const;

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

// ---------------- 配置结构 ----------------
struct GeneralConfig
{
    std::string log_level = "info";
    int perf_interval = 5; // 性能报告周期(秒), 0 关闭打印
    bool display = true;   // 本地窗口显示拼接画面(无 DISPLAY 时自动关闭)
    bool use_rga = true;   // RGA 加速颜色转换/缩放, 失败时自动回退 CPU
};

// 输入源: MP4 文件 / RTSP / RTMP / HTTP-FLV / 摄像头设备(v4l2)
struct SourceConfig
{
    std::string name;
    std::string url;
    bool enable = true;
    bool loop = true;                  // 文件源播放结束后循环
    bool realtime = true;              // 文件源按 pts 节奏读取(模拟实时流)
    std::string rtsp_transport = "tcp";
    int timeout_ms = 5000;             // 打开/读包超时, 超时触发重连
    int reconnect_max_ms = 30000;      // 重连退避上限
};

struct DecoderConfig
{
    std::string backend = "auto"; // auto | mpp | ffmpeg
    int mpp_split_parse = 1;
    int packet_queue = 64;        // 拉流->解码 包队列长度
};

struct ModelConfig
{
    std::string name;
    std::string path;
    std::string labels = "./model/coco_80_labels_list.txt";
    std::string core = "auto"; // auto(实例间轮询绑定 0/1/2) | 0 | 1 | 2 | 0_1 | 0_1_2
    int instances = 3;         // 上下文实例数(rknn_dup_context 复用权重), 决定该模型的最大并发
    float conf_thresh = 0.25f;
    float nms_thresh = 0.45f;
    float weight = 1.0f; // 融合权重
    bool letterbox = true;
};

struct InferConfig
{
    int infer_interval = 1;   // 跳帧推理: 每 N 帧推理一次, 其余帧复用最近一次结果
    double target_fps = 0;    // 每路目标处理帧率, 0 为不限制
    int reuse_max_ms = 500;   // 复用结果的最大时效, 超过则不再绘制旧框
    bool draw_model_boxes = false; // 额外绘制每个模型的原始框(调试用)
    int threads = 0;          // 推理线程池大小, 0 = 所有模型实例数之和
};

struct FusionConfig
{
    std::string method = "weighted"; // nms | weighted | confidence
    float iou_thresh = 0.55f;        // 跨模型关联阈值
    float nms_thresh = 0.45f;        // 融合后 NMS 阈值
    float score_thresh = 0.25f;      // 参与融合的最低分
    int min_votes = 1;               // 至少被几个模型检出才保留(1=并集高召回, N=交集高精度)
    bool class_agnostic = false;
};

struct MosaicConfig
{
    int width = 1920;
    int height = 1080;
    double fps = 25;
    int cols = 0; // 0 = 自动(接近正方形)
    bool show_label = true;
};

struct Gb28181Config
{
    std::string sip_server_ip;
    int sip_server_port = 5060;
    std::string sip_server_id = "34020000002000000001";
    std::string sip_domain = "3402000000";
    std::string device_id = "34020000001320000001";
    std::string channel_id = "34020000001310000001";
    std::string device_name = "rknn-multi-stream";
    std::string password = "12345678";
    std::string local_ip;          // 为空则自动探测
    int local_sip_port = 5060;     // 被占用时自动向后尝试
    int media_port = 0;            // 0 = 系统分配
    int expires = 3600;
    int keepalive = 60;
    int keepalive_max_miss = 3;    // 连续未应答次数, 超过则判定掉线并重新注册
    std::string on_duplicate_invite = "replace"; // replace | reject
};

struct PushConfig
{
    std::string name;
    bool enable = false;
    std::string type = "rtmp";     // rtmp | rtsp | gb28181
    std::string url;
    std::string source = "mosaic"; // mosaic 或通道号 0/1/2/3
    int width = 0;                 // 0 = 与源一致
    int height = 0;
    double fps = 25;
    int bitrate_kbps = 4000;
    int gop = 50;
    std::string encoder = "auto";  // auto | mpp | ffmpeg | ffmpeg:<encoder name>
    bool overlay = true;           // 叠加时间戳/统计信息
    std::string rtsp_transport = "tcp";
    int timeout_ms = 5000;
    int reconnect_max_ms = 30000;
    Gb28181Config gb;
};

struct AppConfig
{
    GeneralConfig general;
    std::vector<SourceConfig> sources;
    DecoderConfig decoder;
    std::vector<ModelConfig> models;
    InferConfig infer;
    FusionConfig fusion;
    MosaicConfig mosaic;
    std::vector<PushConfig> pushes;

    static bool loadFromFile(const std::string &path, AppConfig &cfg, std::string &err);
    // 兼容旧命令行: <model> <source...>
    static AppConfig makeDefault(const std::string &model, const std::vector<std::string> &sources);
    bool validate(std::string &err) const;
    void dump() const;
};

#endif
