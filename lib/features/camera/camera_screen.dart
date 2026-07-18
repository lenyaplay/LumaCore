import 'package:flutter/material.dart';

import '../../core/channels/native_channel.dart';

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  CameraStartResult? _camera;
  String? _error;
  bool _isRecording = false;

  @override
  void initState() {
    super.initState();
    _startCamera();
  }

  Future<void> _startCamera() async {
    try {
      final camera = await NativeChannel.startCamera();
      if (!mounted) return;
      setState(() {
        _camera = camera;
        _error = null;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = 'Camera unavailable: $e';
      });
    }
  }

  Future<void> _toggleRecording() async {
    if (_isRecording) {
      try {
        await NativeChannel.stopRecording();
      } catch (e) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to stop recording: $e')),
        );
      } finally {
        if (mounted) {
          setState(() {
            _isRecording = false;
          });
        }
      }
      return;
    }

    try {
      await NativeChannel.startRecording();
      if (!mounted) return;
      setState(() {
        _isRecording = true;
      });
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Failed to start recording: $e')),
      );
    }
  }

  @override
  void dispose() {
    // Fire-and-forget: dispose() can't await, and there's nothing meaningful
    // to do with a stopCamera/stopRecording failure once the screen is gone.
    if (_isRecording) {
      NativeChannel.stopRecording().catchError((_) {});
    }
    NativeChannel.stopCamera().catchError((_) {});
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Expanded(
          child: Container(
            width: double.infinity,
            color: Colors.black,
            alignment: Alignment.center,
            child: _buildPreview(),
          ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 16),
            child: FloatingActionButton.large(
              onPressed: _camera == null ? null : _toggleRecording,
              tooltip: _isRecording ? 'Stop Recording' : 'Start Recording',
              backgroundColor: _isRecording ? Colors.white : Colors.red,
              child: Icon(
                _isRecording ? Icons.stop : Icons.fiber_manual_record,
                color: _isRecording ? Colors.red : null,
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildPreview() {
    if (_error != null) {
      return Padding(
        padding: const EdgeInsets.all(24),
        child: Text(
          _error!,
          textAlign: TextAlign.center,
          style: const TextStyle(color: Colors.white70),
        ),
      );
    }
    final camera = _camera;
    if (camera == null) {
      return const CircularProgressIndicator(color: Colors.white70);
    }
    // Raw camera passthrough (iOS pull-model, ARCHITECTURE.md §3). GPU shader
    // pipeline lands in Этап 4 — this is the unprocessed camera feed.
    // AspectRatio prevents Texture() from stretching the buffer to fill the
    // container — Texture always sizes to its parent's full constraints.
    return AspectRatio(
      aspectRatio: camera.width / camera.height,
      child: Texture(textureId: camera.textureId),
    );
  }
}
