/*
  puppet_render - renders Puppet Pin animation frames outside VEGAS.

  It links the very same PuppetCore.h the OFX plug-in uses (mesh detection,
  pin binding, ARAP solver, triangle warp), so the preview the AI Motion tool
  shows is what VEGAS will render once the pin keyframes are applied to the
  Puppet Pin effect.

  Usage:
    puppet_render job.json

  job.json (written by aimotion/preview.py):
    {
      "input":  "stage.rgba",          raw straight RGBA, top-down rows
      "width":  1920, "height": 1080,
      "frames": 90,
      "mesh":   {"use_alpha": true, "alpha_threshold": 0.05,
                 "density": 48, "expansion": 3.0},
      "rigidity": 20,
      "background": [32, 32, 36],      optional; omitted = keep alpha
      "draw_pins": false,
      "pins": [ {"anchor": [u, v], "track": [[u, v], ...]}, ... ]
    }

  Pin coordinates are Puppet Pin's normalised frame coordinates: (0, 0) is
  the bottom-left corner, (1, 1) the top-right one. Frames are written to
  stdout back to back as raw RGBA (4 bytes per pixel, top-down rows), ready
  to pipe into ffmpeg (-f rawvideo -pix_fmt rgba). Diagnostics go to stderr.

  The JSON reader below only understands what aimotion writes (objects,
  arrays, numbers, strings, true/false/null); it is not a general parser.
*/

#include "PuppetCore.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON

struct Json {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
  bool b = false;
  double n = 0.0;
  std::string s;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;

  const Json& operator[](const std::string& k) const {
    static const Json none;
    auto it = obj.find(k);
    return it == obj.end() ? none : it->second;
  }
  const Json& operator[](size_t i) const {
    static const Json none;
    return i < arr.size() ? arr[i] : none;
  }
  double num(double def) const { return type == kNum ? n : def; }
  bool boolean(bool def) const { return type == kBool ? b : def; }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : t_(text) {}
  Json parse() {
    Json v = value();
    ws();
    if (i_ != t_.size()) fail("trailing characters");
    return v;
  }

 private:
  const std::string& t_;
  size_t i_ = 0;

  [[noreturn]] void fail(const char* what) {
    std::ostringstream os;
    os << "job.json: " << what << " at offset " << i_;
    throw std::runtime_error(os.str());
  }
  void ws() {
    while (i_ < t_.size() && std::isspace((unsigned char)t_[i_])) ++i_;
  }
  bool eat(char c) {
    ws();
    if (i_ < t_.size() && t_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!eat(c)) fail("unexpected character");
  }
  Json value() {
    ws();
    if (i_ >= t_.size()) fail("unexpected end");
    const char c = t_[i_];
    Json v;
    if (c == '{') {
      ++i_;
      v.type = Json::kObj;
      if (eat('}')) return v;
      do {
        ws();
        Json k = str();
        expect(':');
        v.obj[k.s] = value();
      } while (eat(','));
      expect('}');
    } else if (c == '[') {
      ++i_;
      v.type = Json::kArr;
      if (eat(']')) return v;
      do v.arr.push_back(value());
      while (eat(','));
      expect(']');
    } else if (c == '"') {
      v = str();
    } else if (t_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      v.type = Json::kBool;
      v.b = true;
    } else if (t_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      v.type = Json::kBool;
    } else if (t_.compare(i_, 4, "null") == 0) {
      i_ += 4;
    } else {
      const char* begin = t_.c_str() + i_;
      char* end = nullptr;
      v.n = std::strtod(begin, &end);
      if (end == begin) fail("bad value");
      i_ += (size_t)(end - begin);
      v.type = Json::kNum;
    }
    return v;
  }
  Json str() {
    if (i_ >= t_.size() || t_[i_] != '"') fail("expected string");
    ++i_;
    Json v;
    v.type = Json::kStr;
    while (i_ < t_.size() && t_[i_] != '"') {
      char c = t_[i_++];
      if (c == '\\' && i_ < t_.size()) {
        const char e = t_[i_++];
        switch (e) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'u':  // paths are ASCII-escaped by Python's json.dumps; keep BMP code points as UTF-8
          {
            if (i_ + 4 > t_.size()) fail("bad escape");
            const unsigned cp = (unsigned)std::strtoul(t_.substr(i_, 4).c_str(), nullptr, 16);
            i_ += 4;
            if (cp < 0x80) {
              v.s += (char)cp;
            } else if (cp < 0x800) {
              v.s += (char)(0xC0 | (cp >> 6));
              v.s += (char)(0x80 | (cp & 0x3F));
            } else {
              v.s += (char)(0xE0 | (cp >> 12));
              v.s += (char)(0x80 | ((cp >> 6) & 0x3F));
              v.s += (char)(0x80 | (cp & 0x3F));
            }
            continue;
          }
          default: c = e; break;
        }
      }
      v.s += c;
    }
    if (i_ >= t_.size()) fail("unterminated string");
    ++i_;
    return v;
  }
};

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

// ---------------------------------------------------------------------------

struct Pin {
  puppet::Vec2 anchor;               // normalised
  std::vector<puppet::Vec2> track;  // normalised, one per frame
};

int run(const std::string& jobPath) {
  const Json job = Parser(readFile(jobPath)).parse();
  const int w = (int)job["width"].num(0), h = (int)job["height"].num(0);
  const int frames = (int)job["frames"].num(0);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) throw std::runtime_error("bad width/height");
  if (frames <= 0 || frames > 100000) throw std::runtime_error("bad frame count");

  std::string pixels = readFile(job["input"].s);
  if (pixels.size() != (size_t)w * h * 4) throw std::runtime_error("input size does not match width*height*4");

  // Top-down RGBA buffer seen bottom-up, as PuppetCore expects y up.
  puppet::ImageRef src;
  src.data = &pixels[(size_t)(h - 1) * w * 4];
  src.x1 = 0;
  src.y1 = 0;
  src.x2 = w;
  src.y2 = h;
  src.rowBytes = -(std::ptrdiff_t)w * 4;
  src.nComp = 4;
  src.depth = puppet::kDepthU8;
  src.straightAlpha = true;

  // Mesh exactly as PuppetPin.cpp computePuppet() builds it for a project
  // whose frame size equals the stage size (render scale 1, square pixels).
  const Json& mj = job["mesh"];
  const int density = std::max(4, std::min(400, (int)mj["density"].num(48)));
  puppet::MeshParams mp;
  mp.useAlpha = mj["use_alpha"].boolean(true);
  mp.alphaThreshold = std::max(0.0, std::min(1.0, mj["alpha_threshold"].num(0.05)));
  double cell = (double)w / density;
  cell = std::max(cell, std::sqrt(std::max(1.0, (double)w * h) / 60000.0));
  mp.cellSize = std::max(cell, 1.0);
  mp.expansion = std::max(0.0, std::min(1000.0, mj["expansion"].num(3.0)));
  mp.gridOrigin = puppet::Vec2(0, 0);
  mp.frame = puppet::RectI(0, 0, w, h);
  puppet::Mesh mesh;
  if (!puppet::buildMesh(src, mp, mesh)) throw std::runtime_error("no surface to mesh (is the stage transparent?)");
  const int iterations = std::max(0, std::min(200, (int)job["rigidity"].num(20)));

  std::vector<Pin> pins;
  for (const Json& pj : job["pins"].arr) {
    Pin p;
    p.anchor = puppet::Vec2(pj["anchor"][0].num(0), pj["anchor"][1].num(0));
    for (const Json& t : pj["track"].arr) p.track.push_back(puppet::Vec2(t[0].num(0), t[1].num(0)));
    if ((int)p.track.size() != frames) throw std::runtime_error("every pin track needs one entry per frame");
    pins.push_back(p);
  }
  if (pins.empty() || pins.size() > 20) throw std::runtime_error("need 1..20 pins (Puppet Pin limit)");

  std::vector<puppet::PinBind> binds;
  std::vector<int> bindPin;  // pin index of each bind
  int unbound = 0;
  for (size_t k = 0; k < pins.size(); ++k) {
    const puppet::PinBind b = puppet::bindPin(mesh, puppet::Vec2(pins[k].anchor.x * w, pins[k].anchor.y * h));
    if (b.ok) {
      binds.push_back(b);
      bindPin.push_back((int)k);
    } else {
      ++unbound;
    }
  }
  std::fprintf(stderr, "puppet_render: %dx%d, %d frames, %d triangles, %d/%d pins bound\n", w, h, frames,
               mesh.numTriangles(), (int)binds.size(), (int)pins.size());
  if (unbound) std::fprintf(stderr, "puppet_render: warning: %d pin(s) are off the mesh and do nothing\n", unbound);

  puppet::ArapSolver solver;
  const bool solverOk = !binds.empty() && solver.setup(mesh, binds);

  const bool hasBg = job["background"].type == Json::kArr;
  const float bg[3] = {(float)job["background"][0].num(0), (float)job["background"][1].num(0),
                       (float)job["background"][2].num(0)};
  const bool drawPins = job["draw_pins"].boolean(false);

#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::vector<uint8_t> out((size_t)w * h * 4);
  puppet::ImageRef dst = src;
  dst.data = &out[(size_t)(h - 1) * w * 4];
  const puppet::RectI window(0, 0, w, h);
  std::vector<puppet::Vec2> deformed, targets(binds.size());
  std::vector<puppet::WarpTriangle> tris;

  for (int f = 0; f < frames; ++f) {
    for (size_t i = 0; i < binds.size(); ++i) {
      const puppet::Vec2 t = pins[bindPin[i]].track[f];
      targets[i] = puppet::Vec2(t.x * w, t.y * h);
    }
    deformed = mesh.rest;
    if (solverOk) solver.solve(mesh, binds, targets, iterations, deformed);
    puppet::prepareTriangles(mesh, deformed, tris);
    puppet::clearBand(dst, window, 0, h);
    puppet::warpBand(src, dst, window, tris, 0, h);

    if (drawPins) {
      puppet::Painter paint(dst, window);
      std::vector<puppet::OverlayPin> ov;
      for (size_t k = 0; k < pins.size(); ++k) {
        puppet::OverlayPin o;
        o.p = puppet::Vec2(pins[k].track[f].x * w, pins[k].track[f].y * h);
        o.number = (int)k + 1;
        o.state = puppet::kPinActive;
        ov.push_back(o);
      }
      puppet::drawPins(paint, ov, std::max(1.0, h / 720.0));
    }
    if (hasBg) {
      for (size_t p = 0; p < out.size(); p += 4) {
        const float a = out[p + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) out[p + c] = (uint8_t)std::lround(out[p + c] * a + bg[c] * (1.0f - a));
        out[p + 3] = 255;
      }
    }
    if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size()) throw std::runtime_error("stdout closed");
  }
  std::fflush(stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: puppet_render job.json  (frames go to stdout as raw RGBA)\n");
    return 2;
  }
  try {
    return run(argv[1]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "puppet_render: error: %s\n", e.what());
    return 1;
  }
}
