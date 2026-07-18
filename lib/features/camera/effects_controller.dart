import 'dart:async';

import '../../core/ffi/lumacore_bindings.dart';

/// Feature-level controller for effect params/stats polling — the
/// throttling/UI-facing logic lives here, not in core/ffi/ (which stays a
/// bare binding). See ai_plans/03-ios-metal-render-pipeline.md §9.
class EffectsController {
  EffectsController(this.sessionId);

  final int sessionId;
  Timer? _statsTimer;

  /// Called on every slider onChanged, not onChangeEnd — this is the
  /// "tens of Hz while dragging" case ARCHITECTURE.md §3 calls out as the
  /// reason dart:ffi exists instead of a Platform Channel for this call.
  void updateParams(LumaEffectParamsDart params) {
    LumaCoreBindings.instance.setEffectParams(sessionId, params);
  }

  void startStatsPolling(void Function(LumaStatsDart) onStats) {
    _statsTimer?.cancel();
    // 2-4Hz per ARCHITECTURE.md §3.
    _statsTimer = Timer.periodic(const Duration(milliseconds: 300), (_) {
      onStats(LumaCoreBindings.instance.getStats(sessionId));
    });
  }

  void dispose() {
    _statsTimer?.cancel();
    _statsTimer = null;
  }
}
