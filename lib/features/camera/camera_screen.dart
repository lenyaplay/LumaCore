import 'package:flutter/material.dart';

class CameraScreen extends StatelessWidget {
  const CameraScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Expanded(
          child: Container(
            width: double.infinity,
            color: Colors.black,
            alignment: Alignment.center,
            child: const Padding(
              padding: EdgeInsets.all(24),
              child: Text(
                'Camera preview\n(native texture registration — Этап 2, GPU shader pipeline — Этап 4)',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white70),
              ),
            ),
          ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 16),
            child: FloatingActionButton.large(
              // TODO(Этап 5): lumacore_start_recording / lumacore_stop_recording
              onPressed: null,
              backgroundColor: Colors.red,
              child: const Icon(Icons.fiber_manual_record),
            ),
          ),
        ),
      ],
    );
  }
}
