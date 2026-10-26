# Finished Picture with native game FG - unblocked experiment

Branch: `codex/finished-picture-native-fg-unlocked`, based on `34a1504e` (the Superior Scaling review fixes).

At the author's explicit request, `Late::PausedForGameFrameGeneration()` returns false on this experimental branch. This removes the shared pause gate from all four callers: capture, normal/deferred Finished Picture evaluation, swapchain composition and the native/virtual finished-picture input path. No INI switch or new UI control is required. PR #249's branch is not changed.

The original game-FG detector is unchanged. So are `FinishedInputReady`, newest-slot selection, deferred retirement, device checks, and the rule against waiting for incomplete work from another queue at present. No shader, guide scaling, Superior gain, private DLSS, exposure or FG routing changes are included.

This is an unblock test, not the proposed integration before native Streamline interpolation. The current presentation point may include generated frames, and the existing checks may skip frames whose input is not ready. This branch does not establish that colour/depth/MV correspond correctly to all game-generated frames.

Test the game's native FG with `NR Pass at: Finished Picture`, including moving scenes, HUD/menus, FG toggle, loading, HDR and resolution changes. Compare with the unmodified build. The Finished Picture game-FG pause message should no longer be triggered by this gate; other availability/queue checks can still prevent NR from running.

Local validation passed: the existing WARP queue regression (readiness, newest-slot selection, device removal and resize waits) and finished-colour tests (SDR/scRGB/HDR10, residual transfer, alpha and invalid data). The detector's original unit tests still pass because the detector itself is unchanged; this branch overrides the runtime pause decision only. A full Release build is also requested. Real Crimson Desert stability, image quality, frame pacing and GPU cost require in-game testing; no in-game performance result is claimed.
