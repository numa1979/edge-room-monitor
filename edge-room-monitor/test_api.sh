#!/bin/bash
# Test script for detection API endpoint

echo "Testing detection API endpoint..."
echo ""
echo "1. Testing /api/detections endpoint:"
curl -s http://192.168.0.212:8080/api/detections | python3 -m json.tool
echo ""
echo ""
echo "2. Testing root endpoint (should serve MJPEG):"
curl -s -I http://192.168.0.212:8080/ | head -5
echo ""
