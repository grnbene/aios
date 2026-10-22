// How tools/face-render-check.cpp paints a face Runtime into a buffer.
inline void face_render_paint(aios::userland::face::Runtime& rt, std::vector<uint32_t>& px, int w, int h) {
  rt.paint(px, w, h, 0, 0);
}
