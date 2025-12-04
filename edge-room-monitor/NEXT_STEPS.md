# Next Steps for Tap-to-Track Implementation

## Current Status ✓
- Phase 1: Detection API endpoint implemented
- Ready for testing

## Testing Phase 1

Run on Jetson:
```bash
cd /home/numa/edge-room-monitor/edge-room-monitor
sudo ./start_app.sh
```

Test from another terminal:
```bash
# Test API
curl http://192.168.0.212:8080/api/detections

# Or use the test script
bash edge-room-monitor/test_api.sh
```

## Phase 2: Tap-to-Track Web Interface

### 2.1 Create Interactive Web UI
- Modify `ui/mjpeg_viewer.html` to:
  - Fetch detections from `/api/detections`
  - Draw clickable bounding boxes on canvas overlay
  - Handle click events to select person
  - Send selected tracking_id to backend

### 2.2 Add Selection API
- Add `/api/select` endpoint to receive tracking_id
- Store selected person's tracking_id in backend
- Modify OSD to highlight selected person differently

### 2.3 Visual Feedback
- Change bounding box color for selected person
- Add label showing "TRACKING: Person #X"
- Keep tracking even when person moves

## Phase 3: Re-Identification (Re-ID)

### 3.1 Feature Extraction
- Extract visual features from selected person's bounding box
- Use DeepStream's feature extraction or custom model
- Store feature vector for selected person

### 3.2 Re-ID When Person Returns
- When person leaves frame (tracking lost)
- Compare new detections against stored features
- Re-assign tracking_id if match found
- Continue tracking with same ID

### 3.3 Feature Matching
- Implement cosine similarity or L2 distance
- Set threshold for re-identification
- Handle multiple people with similar features

## Technical Notes

- All inference must use DeepStream (not Python) for performance
- Feature extraction can use ResNet or similar backbone
- Consider using DeepStream's nvdsanalytics for zone tracking
- May need secondary nvinfer for feature extraction model
