# Detection API Implementation

## Status: Ready for Testing

### What Was Implemented

1. **DeepStream Metadata Extraction**
   - Added extraction of `NvDsBatchMeta` from GStreamer buffers
   - Extracts tracking_id, class_id, confidence, and bounding box coordinates
   - Processes all detected objects in each frame

2. **Thread-Safe Detection Storage**
   - Created `DetectionStore` class with mutex protection
   - Stores latest detections from each frame
   - Provides thread-safe `get()` method for HTTP requests

3. **JSON API Endpoint**
   - Added `/api/detections` endpoint
   - Returns JSON format:
     ```json
     {
       "detections": [
         {
           "tracking_id": 1,
           "class_id": 0,
           "confidence": 0.85,
           "bbox": {
             "left": 100.5,
             "top": 200.3,
             "width": 150.2,
             "height": 300.1
           }
         }
       ]
     }
     ```

4. **HTTP Request Routing**
   - Added `parse_http_path()` to extract request path
   - Routes `/api/*` requests to API handler
   - Routes other requests to MJPEG stream handler
   - Added CORS header for cross-origin requests

5. **CMake Configuration**
   - Added DeepStream library directory: `/opt/nvidia/deepstream/deepstream/lib`
   - Linked required libraries: `nvdsgst_meta` and `nvds_meta`
   - Added DeepStream includes: `/opt/nvidia/deepstream/deepstream/sources/includes`

### Testing Instructions

1. **Build and Run**
   ```bash
   cd /home/numa/edge-room-monitor/edge-room-monitor
   sudo ./start_app.sh
   ```

2. **Test API Endpoint**
   ```bash
   # From another terminal
   curl http://192.168.0.212:8080/api/detections | python3 -m json.tool
   ```

3. **Test MJPEG Stream** (should still work)
   ```bash
   # Open in browser
   http://192.168.0.212:8080/
   ```

### Expected Behavior

- MJPEG stream continues to work at root path `/`
- New API endpoint at `/api/detections` returns JSON with current detections
- Each detection includes tracking ID from nvtracker
- Bounding boxes are in pixel coordinates
- API updates in real-time as new frames are processed

### Next Steps (Phase 2)

Once API is confirmed working:
1. Create web interface with clickable bounding boxes
2. Implement tap-to-track functionality
3. Store selected person's tracking_id
4. Highlight selected person in video stream

### Troubleshooting

If segmentation fault occurs:
- Check that DeepStream libraries are properly linked
- Verify `gst_buffer_get_nvds_batch_meta()` is available
- Ensure nvtracker is producing object metadata
- Check that pipeline includes nvtracker element

If API returns empty detections:
- Verify person detection is working in MJPEG stream
- Check that nvtracker is assigning tracking IDs
- Ensure metadata is being attached to buffers
