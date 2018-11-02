#include <iostream>
#include <fstream>
#include "cvDnnDetector.h"

namespace ff_dynamic {
/////////////////////////////////
// [Register - auto, cvDnnDetect] There is no data output but only peer events
static DavImplRegister s_cvDnnDetectReg(DavWaveClassCvDnnDetect(), vector<string>({"auto", "cvDnnDetect"}), {},
                                        [](const DavWaveOption & options) -> unique_ptr<DavImpl> {
                                            unique_ptr<CvDnnDetect> p(new CvDnnDetect(options));
                                            return p;
                                        });
const DavRegisterProperties & CvDnnDetect::getRegisterProperties() const noexcept {
    return s_cvDnnDetectReg.m_properties;
}

////////////////////////////////////
//  [event process]
int CvDnnDetect::processChangeConfThreshold(const CvDynaEventChangeConfThreshold & e) {
    m_dps.m_confThreshold = e.m_confThreshold;
    return 0;
}

////////////////////////////////////
//  [construct - destruct - process]
int CvDnnDetect::onConstruct() {
    LOG(INFO) << m_logtag << "start creating CvDnnDetect " << m_options.dump();
    std::function<int (const CvDynaEventChangeConfThreshold &)> f =
        [this] (const CvDynaEventChangeConfThreshold & e) {return processChangeConfThreshold(e);};
    m_implEvent.registerEvent(f);

    m_dps.m_detectorType = m_options.get("detector_type");
    m_dps.m_detectorFrameworkTag = m_options.get("detector_framework_tag");
    m_dps.m_modelPath = m_options.get("model_path");
    m_dps.m_configPath = m_options.get("config_path");
    m_dps.m_classnamePath = m_options.get("classname_path");
    m_options.getInt("backend_id", m_dps.m_backendID);
    m_options.getInt("target_id", m_dps.m_targetId);
    m_options.getDouble("scale_factor", m_dps.m_scaleFactor);
    m_options.getBool("swap_rb", m_dps.m_bSwapRb);
    m_options.getInt("width", m_dps.m_width);
    m_options.getInt("height", m_dps.m_height);
    m_options.getDouble("conf_threshold", m_dps.m_confThreshold);
    m_options.getDouble("conf_threshold", m_dps.m_confThreshold);
    vector<double> means;
    m_options.getDoubleArray("means", means);
    for (auto & m : means)
        m_dps.m_means.emplace_back(m);

    // what about fail ?
    try {
        m_net = readNet(m_dps.m_modelPath, m_dps.m_configPath, m_dps.m_detectorFrameworkTag);
    } catch (const std::exception & e) {
        string detail = "Fail create cv dnn net model. model path " + m_dps.m_modelPath +
            ", confit path" +  m_dps.m_configPath +  e.what()
        ERRORIT(DAV_ERROR_IMPL_ON_CONSTRUCT, detail);
        return DAV_ERROR_IMPL_ON_CONSTRUCT;
    }
    m_net.setPreferableBackend(m_dps.m_backendId);
    m_net.setPreferableTarget(m_dps.m_targetId);

    std::ifstream ifs(m_dps.m_classnamePath.c_str());
    if (!ifs.is_open()) {
        ERRORIT(DAV_ERROR_IMPL_ON_CONSTRUCT, "fail to open class name file " + m_dps.m_classnamePath);
        return DAV_ERROR_IMPL_ON_CONSTRUCT;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        m_classNames.emplace_back(line);
    }
    LOG(INFO) << m_logtag << "CvDnnDetect create done: " << m_dps;
    return 0;
}

int CvDnnDetect::onDestruct() {
    LOG(INFO) << m_logtag << "CvDnnDetect Destruct";
    return 0;
}

////////////////////////////////////
//  [dynamic initialization]

int CvDnnDetect::onDynamicallyInitializeViaTravelStatic(DavProcCtx & ctx) {
    DavImplTravel::TravelStatic & in = m_inputTravelStatic.at(ctx.m_froms[0]);
    if (!in.m_codecpar && (in.m_pixfmt == AV_PIX_FMT_NONE)) {
        ERRORIT(DAV_ERROR_TRAVEL_STATIC_INVALID_CODECPAR,
                m_logtag + "dehaze cannot get valid codecpar or videopar");
        return DAV_ERROR_TRAVEL_STATIC_INVALID_CODECPAR;
    }
    /* set output infos */
    m_timestampMgr.clear();
    m_outputTravelStatic.clear();
    m_timestampMgr.insert(std::make_pair(ctx.m_froms[0], DavImplTimestamp(in.m_timebase, in.m_timebase)));
    /* output no data stream, so no output travel static info */
    m_bDynamicallyInitialized = true;
    return 0;
}

////////////////////////////
//  [dynamic initialization]

int CvDnnDetect::onProcess(DavProcCtx & ctx) {
    ctx.m_expect.m_expectOrder = {EDavExpect::eDavExpectAnyOne};
    if (!ctx.m_inBuf) {
        ERRORIT(DAV_ERROR_IMPL_UNEXPECT_EMPTY_INBUF, "CvDnnDetect should always have input");
        return DAV_ERROR_IMPL_UNEXPECT_EMPTY_INBUF;
    }

    int ret = 0;
    /* ref frame is a frame ref to original frame (data shared),
       but timestamp is convert to current impl's timebase */
    auto inFrame = ctx.m_inRefFrame;
    if (!inFrame) {
        LOG(INFO) << m_logtag << "cv dnn detector reciving flush frame";
        ctx.m_bInputFlush = true;
        /* no flush needed, so just return EOF */
        return AVERROR_EOF;
    }

    // convert this frame to opencv Mat
    CHECK((enum AVPixelFormat)inFrame->format == AV_PIX_FMT_YUV420P);
    cv::Mat yuvMat;
    yuvMat.create(inFrame->height * 3 / 2, inFrame->width, CV_8UC1);
    for (int k=0; k < inFrame->height; k++)
        memcpy(yuvMat.data + k * inFrame->width, inFrame->data[0] + k * inFrame->linesize[0], inFrame->width);
    const auto u = yuvMat.data + inFrame->width * inFrame->height;
    const auto v = yuvMat.data + inFrame->width * inFrame->height * 5 / 4 ;
    for (int k=0; k < inFrame->height/2; k++) {
        memcpy(u + k * inFrame->width/2, inFrame->data[1] + k * inFrame->linesize[1], inFrame->width/2);
        memcpy(v + k * inFrame->width/2, inFrame->data[2] + k * inFrame->linesize[2], inFrame->width/2);
    }

    cv::Mat image, blob;
    if (m_dps.m_bSwapRb)
        cv::cvtColor(yuvMat, image, CV_YUV2RGB_I420);
    else
        cv::cvtColor(yuvMat, image, CV_YUV2BGR_I420);

    cv::Size inpSize(m_dps.m_width > 0 ? m_dps.m_width : image.cols,
                     m_dps.m_height > 0 ? m_dps.m_height : image.rows);
    blobFromImage(image, blob, m_dps.m_scaleFactor, isSize, m_dps.m_means, false, false);
    m_net.setInptu(blob);
    if (m_net.getLayer(0)->outputNameToIndex("im_info") != -1) { // Faster-RCNN or R-FCN
        resize(image, image, inpSize);
        Mat imInfo = (Mat_<float>(1, 3) << inpSize.height, inpSize.width, 1.6f);
        net.setInput(imInfo, "im_info");
    }

    std::vector<cv::Mat> outs;
    m_net.forward(outs, getOutputsNames(net));

    /* prepare output events*/
    auto detectEvent = make_shared<CvDnnDetectEvent>();
    detectEvent.m_framePts = inFrame->pts;
    postprocess(image, outs, detectEvent);

    detectEvent.m_inferTime =
    // Put efficiency information.
    std::vector<double> layersTimes;
    const double freq = cv::getTickFrequency() / 1000;
    const double t = m_net.getPerfProfile(layersTimes) / freq;

    double m_inferTime = 0.0;
    struct DetectResult {
        string m_className;
        double m_confidence;
        DavRect m_rect; /* not used for classify */
    };
    vector<DetectResult> m_results;

    ctx.m_pubEvents.empalce_back(detectEvent);
    /* No travel static needed for detectors, just events */
    return 0;
}

////////////
// [helpers]
int CvDnnDetect::postprocess(const cv::Mat & image, const vector<cv::Mat> & outs,
                             shared_ptr<CvDnnDetectEvent> & detectEvent) {
    vector<double> layersTimes;
    const double freq = cv::getTickFrequency() / 1000;
    detectEvent.m_inferTime = m_net.getPerfProfile(layersTimes) / freq;
    detectEvent.m_detectorType = m_dps.m_detectorType;
    detectEvent.m_detectorFrameworkTag = m_dps.m_detectorFrameworkTag;

    /* result assign */
    vector<int> outLayers = net.getUnconnectedOutLayers();
    string outLayerType = net.getLayer(outLayers[0])->type;
    if (net.getLayer(0)->outputNameToIndex("im_info") != -1) { // Faster-RCNN or R-FCN
        // Network produces output blob with a shape 1x1xNx7 where N is a number of detections.
        // each detection is: [batchId, classId, confidence, left, top, right, bottom]
        CHECK(outs.size() == 1) << "Faster-Rcnn or R-FCN output should have size 1";
        const float* data = (float*)outs[0].data;
        for (size_t i=0; i < outs[0].total(); i+=7) {
            CvDnnDetectEvent::DetectResult result;
            result.m_confidence = data[i + 2];
            if (result.confidence > m_dps.m_confThreshold) {
                result.m_rect.x = (int)data[i + 3];
                result.m_rect.y = (int)data[i + 4];
                const int right = (int)data[i + 5];
                const int bottom = (int)data[i + 6];
                result.m_rect.width = right - result.m_rect.x + 1;
                result.m_rect.height = bottom - result.m_rect.y + 1;
                const int classId = (int)(data[i + 1]) - 1; // classId 0 is background
                if (classId < (int)m_classNames.size())
                    result.m_className = m_classNames[classId];
            }
            detectEvent.emplace_back(result);
        }
    } else if (outLayerType == "DetectionOutput") {
        // Network produces output blob with a shape 1x1xNx7 where N is a number of detections。
        // each detection: [batchId, classId, confidence, left, top, right, bottom]
        CHECK(outs.size() == 1) << "DetectionOutput should have size one";
        const float* data = (float*)outs[0].data;
        for (size_t i=0; i < outs[0].total(); i+=7) {
            CvDnnDetectEvent::DetectResult result;
            result.m_confidence = data[i + 2];
            if (result.confidence > m_dps.m_confThreshold) {
                result.m_rect.x = (int)(data[i + 3] * image.cols);
                result.m_rect.y = (int)(data[i + 4] * image.rows);
                const int right = (int)(data[i + 5] * image.cols);
                const int bottom = (int)(data[i + 6] * image.rows);
                result.m_rect.w = right - result.m_rect.x + 1;
                result.m_rect.h = bottom - result.m_rect.y + 1;
                const int classId = (int)(data[i + 1]) - 1; // classId 0 is background
                if (classId < (int)m_classNames.size())
                    result.m_className = m_classNames[classId];
            }
            detectEvent.emplace_back(result);
        }
    } else if (outLayerType == "Region") {
        for (size_t i = 0; i < outs.size(); ++i) {
            // Network produces output blob with a shape NxC where N is a number of detected objects
            // and C is a number of classes + 4; 4 numbers are [center_x, center_y, width, height]
            float* data = (float*)outs[i].data;
            for (int j = 0; j < outs[i].rows; ++j, data += outs[i].cols) {
                Mat scores = outs[i].row(j).colRange(5, outs[i].cols);
                Point classIdPoint;
                minMaxLoc(scores, 0, &result.m_confidence, 0, &classIdPoint);
                if (result.m_confidence > m_dps.m_confThreshold) {
                    int centerX = (int)(data[0] * image.cols);
                    int centerY = (int)(data[1] * image.rows);
                    result.m_rect.w = (int)(data[2] * image.cols);
                    result.m_rect.h = (int)(data[3] * image.rows);
                    result.m_rect.x = centerX - result.m_rect.w / 2;
                    result.m_rect.y = centerY - result.m_rect.h / 2;
                    if (classIdPoint.x < (int)m_classNames.size())
                        result.m_className = m_classNames[classIdPoint.x];
                }
            }
            detectEvent.emplace_back(result);
        }
    } else {
        LOG(ERROR) << m_logtag << "Unknown output layer type: " << outLayerType;
        return -1;
    }
    return 0;
}

std::vector<cv::String> & CvDnnDetect::getOutputsNames() {
    if (m_outBlobNames.empty()) {
        vector<int> outLayers = m_net.getUnconnectedOutLayers();
        vector<cv::String> layersNames = m_net.getLayerNames();
        m_outBlobNames.resize(outLayers.size());
        for (size_t i = 0; i < outLayers.size(); ++i)
            m_outBlobNames[i] = layersNames[outLayers[i] - 1];
    }
    return m_outBlobNames;
}

std::ostream & operator<<(std::ostream & os, const DetectParams & p) {
    os << "[dectorType " << p.m_detectorType << ", detectorFrameworkTag " << p.m_detectorFrameworkTag
       << ", modelPath " << p.m_modelPath << ", configPath " << p.m_configPath
       << ", classnamePath " << p.m_classnamePath << ", backendId " << p.m_backendId
       << ", targetId " << p.m_targetId << ", scaleFactor " << p.m_scaleFactor
       << ", means " << Scalar << ", bSwapRb " << p.m_bSwapRb << ", width " << p.m_width
       << ", height " << p.m_height << ", confThreshold " << m_confThreshold;
    return os;
}

} // namespace ff_dynamic