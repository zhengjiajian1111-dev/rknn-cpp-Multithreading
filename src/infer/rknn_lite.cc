#include "infer/rknn_lite.hpp"

#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iterator>

#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"
#include "infer/preprocess.h"
#include "opencv2/imgproc/imgproc.hpp"

static std::vector<unsigned char> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::string shapeStr(const rknn_tensor_attr &attr)
{
    std::string s;
    for (uint32_t i = 0; i < attr.n_dims; ++i)
        s += (i ? "x" : "") + std::to_string(attr.dims[i]);
    return s;
}

RknnLite::RknnLite(const ModelConfig &cfg, int instance_id, std::shared_ptr<const std::vector<std::string>> labels,
                   bool use_rga)
    : cfg_(cfg), instance_id_(instance_id), labels_(std::move(labels)), use_rga_(use_rga)
{
    st_pre_ = PerfMonitor::instance().stat(cfg_.name + ".pre");
    st_npu_ = PerfMonitor::instance().stat(cfg_.name + ".npu");
    st_post_ = PerfMonitor::instance().stat(cfg_.name + ".post");
}

RknnLite::~RknnLite()
{
    if (ctx_ready_)
        rknn_destroy(ctx_);
}

int RknnLite::init(RknnLite *master, rknn_core_mask core_mask)
{
    int ret;
    if (master)
    {
        // 权重复用: 多个上下文共享同一份模型权重
        ret = rknn_dup_context(&master->ctx_, &ctx_);
    }
    else
    {
        std::vector<unsigned char> model = readFile(cfg_.path);
        if (model.empty())
        {
            LOGE("[%s] cannot read model file %s", cfg_.name.c_str(), cfg_.path.c_str());
            return -1;
        }
        // rknn_init 会拷贝所需数据, 返回后即可释放模型文件内存
        ret = rknn_init(&ctx_, model.data(), (uint32_t)model.size(), 0, nullptr);
    }
    if (ret < 0)
    {
        LOGE("[%s#%d] rknn_init/dup failed ret=%d", cfg_.name.c_str(), instance_id_, ret);
        return -1;
    }
    ctx_ready_ = true;

    if (core_mask != RKNN_NPU_CORE_AUTO)
    {
        ret = rknn_set_core_mask(ctx_, core_mask);
        if (ret < 0)
            LOGW("[%s#%d] rknn_set_core_mask(%d) failed ret=%d (single-core NPU?), use auto", cfg_.name.c_str(),
                 instance_id_, (int)core_mask, ret);
    }

    if (!master)
    {
        rknn_sdk_version version;
        if (rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == RKNN_SUCC)
            LOGI("[%s] rknn sdk %s, driver %s", cfg_.name.c_str(), version.api_version, version.drv_version);
    }

    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0)
    {
        LOGE("[%s] query io num failed ret=%d", cfg_.name.c_str(), ret);
        return -1;
    }
    if (io_num_.n_input != 1 || io_num_.n_output != 3)
    {
        LOGE("[%s] expect yolov5 model with 1 input / 3 outputs, got %u / %u", cfg_.name.c_str(), io_num_.n_input,
             io_num_.n_output);
        return -1;
    }

    input_attrs_.resize(io_num_.n_input);
    for (uint32_t i = 0; i < io_num_.n_input; i++)
    {
        memset(&input_attrs_[i], 0, sizeof(rknn_tensor_attr));
        input_attrs_[i].index = i;
        if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr)) < 0)
            return -1;
    }
    output_attrs_.resize(io_num_.n_output);
    out_zps_.clear();
    out_scales_.clear();
    for (uint32_t i = 0; i < io_num_.n_output; i++)
    {
        memset(&output_attrs_[i], 0, sizeof(rknn_tensor_attr));
        output_attrs_[i].index = i;
        if (rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr)) < 0)
            return -1;
        const rknn_tensor_attr &a = output_attrs_[i];
        if (a.n_dims != 4 || a.fmt == RKNN_TENSOR_NHWC)
        {
            LOGE("[%s] output %u shape %s fmt %d not supported (need NCHW)", cfg_.name.c_str(), i,
                 shapeStr(a).c_str(), (int)a.fmt);
            return -1;
        }
        if (a.type != RKNN_TENSOR_INT8)
            LOGW("[%s] output %u is not int8, post-process expects an int8 quantized model", cfg_.name.c_str(), i);
        grid_hw_[i][0] = a.dims[2];
        grid_hw_[i][1] = a.dims[3];
        out_zps_.push_back(a.zp);
        out_scales_.push_back(a.scale);
    }
    // 输出通道 = 3 * (5 + 类别数)
    num_classes_ = (int)output_attrs_[0].dims[1] / 3 - 5;
    if (num_classes_ <= 0)
    {
        LOGE("[%s] bad output channel %u", cfg_.name.c_str(), output_attrs_[0].dims[1]);
        return -1;
    }

    const rknn_tensor_attr &in = input_attrs_[0];
    if (in.fmt == RKNN_TENSOR_NCHW)
    {
        model_c_ = in.dims[1];
        model_h_ = in.dims[2];
        model_w_ = in.dims[3];
    }
    else
    {
        model_h_ = in.dims[1];
        model_w_ = in.dims[2];
        model_c_ = in.dims[3];
    }
    if (model_c_ != 3)
    {
        LOGE("[%s] model input channel %d != 3", cfg_.name.c_str(), model_c_);
        return -1;
    }
    input_.create(model_h_, model_w_, CV_8UC3);

    if (!master)
        LOGI("[%s] input %dx%dx%d, outputs %s / %s / %s, classes=%d, labels=%zu", cfg_.name.c_str(), model_w_,
             model_h_, model_c_, shapeStr(output_attrs_[0]).c_str(), shapeStr(output_attrs_[1]).c_str(),
             shapeStr(output_attrs_[2]).c_str(), num_classes_, labels_ ? labels_->size() : 0);
    LOGI("[%s#%d] ready, core_mask=%d", cfg_.name.c_str(), instance_id_, (int)core_mask);
    return 0;
}

int RknnLite::interf(const cv::Mat &ori_img, detect_result_group_t &result)
{
    std::lock_guard<std::mutex> lock(mtx_);
    memset(&result, 0, sizeof(result));
    if (!ctx_ready_ || ori_img.empty())
        return -1;

    // 1. 前处理: BGR -> RGB + 缩放到模型输入尺寸(RGA 或 OpenCV)
    LetterBox lb;
    {
        ScopedTimer t(st_pre_);
        if (!preprocess(ori_img, input_, cfg_.letterbox, use_rga_, lb))
            return -1;
    }

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = model_w_ * model_h_ * model_c_;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0;
    inputs[0].buf = input_.data;

    rknn_output outputs[3];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < 3; i++)
        outputs[i].want_float = 0;

    // 2. NPU 推理
    {
        ScopedTimer t(st_npu_);
        int ret = rknn_inputs_set(ctx_, io_num_.n_input, inputs);
        if (ret < 0)
        {
            LOGE("[%s#%d] rknn_inputs_set failed ret=%d", cfg_.name.c_str(), instance_id_, ret);
            return -1;
        }
        ret = rknn_run(ctx_, nullptr);
        if (ret < 0)
        {
            LOGE("[%s#%d] rknn_run failed ret=%d", cfg_.name.c_str(), instance_id_, ret);
            return -1;
        }
        ret = rknn_outputs_get(ctx_, io_num_.n_output, outputs, nullptr);
        if (ret < 0)
        {
            LOGE("[%s#%d] rknn_outputs_get failed ret=%d", cfg_.name.c_str(), instance_id_, ret);
            return -1;
        }
    }

    // 3. 后处理: 解码 + NMS -> detect_result_group_t
    {
        ScopedTimer t(st_post_);
        static const std::vector<std::string> kNoLabels;
        post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf, model_h_, model_w_,
                     grid_hw_, num_classes_, cfg_.conf_thresh, cfg_.nms_thresh, lb, out_zps_, out_scales_,
                     labels_ ? *labels_ : kNoLabels, &result);
    }
    rknn_outputs_release(ctx_, io_num_.n_output, outputs);
    return 0;
}

void RknnLite::drawResults(cv::Mat &img, const detect_result_group_t &group, const cv::Scalar &color)
{
    char text[64];
    for (int i = 0; i < group.count; i++)
    {
        const detect_result_t &r = group.results[i];
        cv::rectangle(img, cv::Point(r.box.left, r.box.top), cv::Point(r.box.right, r.box.bottom), color, 1);
        snprintf(text, sizeof(text), "%s %.0f%%", r.name, r.prop * 100);
        cv::putText(img, text, cv::Point(r.box.left, r.box.bottom + 12), cv::FONT_HERSHEY_SIMPLEX, 0.4, color, 1);
    }
}
