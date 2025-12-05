#include <algorithm>
#include <cstring>
#include <iostream>
#include "nvdsinfer_custom_impl.h"

// YOLOv8 output: [1, 84, 8400]
// 84 = 4 bbox (x, y, w, h) + 80 class scores
// 8400 = number of anchor points

// Simple NMS implementation
static float compute_iou(const NvDsInferParseObjectInfo& a, const NvDsInferParseObjectInfo& b) {
    float x1 = std::max(a.left, b.left);
    float y1 = std::max(a.top, b.top);
    float x2 = std::min(a.left + a.width, b.left + b.width);
    float y2 = std::min(a.top + a.height, b.top + b.height);
    
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);
    float area_a = a.width * a.height;
    float area_b = b.width * b.height;
    float union_area = area_a + area_b - intersection;
    
    return (union_area > 0.0f) ? (intersection / union_area) : 0.0f;
}

static void apply_nms(std::vector<NvDsInferParseObjectInfo>& objects, float iou_threshold) {
    // Sort by confidence (descending)
    std::sort(objects.begin(), objects.end(), 
              [](const NvDsInferParseObjectInfo& a, const NvDsInferParseObjectInfo& b) {
                  return a.detectionConfidence > b.detectionConfidence;
              });
    
    std::vector<bool> suppressed(objects.size(), false);
    
    for (size_t i = 0; i < objects.size(); ++i) {
        if (suppressed[i]) continue;
        
        for (size_t j = i + 1; j < objects.size(); ++j) {
            if (suppressed[j]) continue;
            
            float iou = compute_iou(objects[i], objects[j]);
            if (iou > iou_threshold) {
                suppressed[j] = true;
            }
        }
    }
    
    // Remove suppressed objects
    std::vector<NvDsInferParseObjectInfo> filtered;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!suppressed[i]) {
            filtered.push_back(objects[i]);
        }
    }
    objects = std::move(filtered);
}

extern "C" bool NvDsInferParseCustomYoloV8(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList) {
    
    if (outputLayersInfo.empty()) {
        std::cerr << "[YOLOv8] No output layers" << std::endl;
        return false;
    }

    const NvDsInferLayerInfo& layer = outputLayersInfo[0];
    
    // YOLOv8 output shape: [1, 84, 8400]
    // layer.inferDims.d[0] = 84
    // layer.inferDims.d[1] = 8400
    
    if (layer.inferDims.numDims != 2) {
        std::cerr << "[YOLOv8] Unexpected dims: " << layer.inferDims.numDims << std::endl;
        return false;
    }
    
    const int numAttrs = layer.inferDims.d[0];  // 84
    const int numAnchors = layer.inferDims.d[1]; // 8400
    
    if (numAttrs != 84) {
        std::cerr << "[YOLOv8] Expected 84 attributes, got " << numAttrs << std::endl;
        return false;
    }
    
    const float* data = static_cast<const float*>(layer.buffer);
    const int numClasses = 80;
    const float confThreshold = 0.4f;  // 信頼度閾値: 40%以上で検出（横たわり時も安定検出）
    const int personClassId = 0;  // COCO person class
    
    // std::cout << "[YOLOv8] Processing " << numAnchors << " detections" << std::endl;
    
    for (int i = 0; i < numAnchors; ++i) {
        // YOLOv8 output is transposed: data is organized as [attr][anchor]
        // data[0 * numAnchors + i] = x for anchor i
        // data[1 * numAnchors + i] = y for anchor i
        // etc.
        
        // Coordinates are normalized (0-1), need to scale to image size
        float cx = data[0 * numAnchors + i] * networkInfo.width;
        float cy = data[1 * numAnchors + i] * networkInfo.height;
        float w = data[2 * numAnchors + i] * networkInfo.width;
        float h = data[3 * numAnchors + i] * networkInfo.height;
        
        // Find max class score
        float maxScore = 0.0f;
        int maxClass = -1;
        for (int c = 0; c < numClasses; ++c) {
            float score = data[(4 + c) * numAnchors + i];
            if (score > maxScore) {
                maxScore = score;
                maxClass = c;
            }
        }
        
        // Filter: only person class and above threshold
        if (maxScore < confThreshold || maxClass != personClassId) {
            continue;
        }
        
        // Convert from center format to corner format
        float x1 = cx - w / 2.0f;
        float y1 = cy - h / 2.0f;
        float x2 = cx + w / 2.0f;
        float y2 = cy + h / 2.0f;
        
        // Clamp to image bounds
        x1 = std::max(0.0f, std::min(x1, static_cast<float>(networkInfo.width)));
        y1 = std::max(0.0f, std::min(y1, static_cast<float>(networkInfo.height)));
        x2 = std::max(0.0f, std::min(x2, static_cast<float>(networkInfo.width)));
        y2 = std::max(0.0f, std::min(y2, static_cast<float>(networkInfo.height)));
        
        float boxW = x2 - x1;
        float boxH = y2 - y1;
        
        if (boxW < 1.0f || boxH < 1.0f) {
            continue;
        }
        
        NvDsInferParseObjectInfo obj{};
        obj.classId = maxClass;
        obj.detectionConfidence = maxScore;
        obj.left = x1;
        obj.top = y1;
        obj.width = boxW;
        obj.height = boxH;
        
        objectList.push_back(obj);
    }
    
    // Apply NMS to remove duplicate detections
    if (!objectList.empty()) {
        apply_nms(objectList, 0.45f);
    }
    
    // std::cout << "[YOLOv8] Detected " << objectList.size() << " person(s)" << std::endl;
    
    return true;
}

extern "C" bool NvDsInferParseYoloV8(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList) {
    return NvDsInferParseCustomYoloV8(outputLayersInfo, networkInfo, detectionParams, objectList);
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseYoloV8);
