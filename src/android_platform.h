#pragma once

namespace materializr {

// Android-only startup hook (no-op on desktop). Run ONCE before constructing
// Application. It:
//   * points $HOME at app-private internal storage so SettingsIO::defaultPath()
//     writes a readable/writable settings file (no code change there);
//   * chdir()s there and extracts the bundled TTFs so resolveBundledFont()'s
//     cwd-relative "assets/fonts/" lookup succeeds (no code change there);
//   * extracts the bundled OpenCASCADE resource tree and sets the CSF_* env vars
//     OCCT consults for unit/message/STEP/IGES data.
void androidInitRuntime();

// Ask MaterializrActivity to switch to the supported display refresh mode
// closest to `fps` (Settings > Rendering). The native render loop separately
// caps itself to the same value, so a lower tier holds even if the system
// ignores the display request.
void androidSetFrameRateLimit(int fps);

} // namespace materializr
