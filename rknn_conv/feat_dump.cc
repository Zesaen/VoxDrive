// feat_dump.cc — 用生产同款 sherpa_onnx::FeatureExtractor 导出 zipformer fbank
// 用法: feat_dump <in.wav> <out.f32bin>   （16k/mono/s16 wav）
// 输出: int32 T, int32 dim, T*dim float32
#include "sherpa-onnx/csrc/features.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static bool read_wav16(const char* path, std::vector<float>& out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  unsigned char hdr[12];
  if (fread(hdr, 1, 12, f) != 12) { fclose(f); return false; }
  // RIFF 头：找 fmt 与 data 块
  int32_t channels = 1, rate = 16000, bits = 16;
  bool have_data = false;
  int64_t data_size = 0;
  while (!have_data) {
    unsigned char chunk_hdr[8];
    if (fread(chunk_hdr, 1, 8, f) != 8) break;
    int32_t sz = chunk_hdr[4] | (chunk_hdr[5] << 8) | (chunk_hdr[6] << 16) |
                 ((int32_t)chunk_hdr[7] << 24);
    if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      if (fread(fmt, 1, 16, f) != 16) break;
      channels = fmt[2] | (fmt[3] << 8);
      rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((int32_t)fmt[7] << 24);
      bits = fmt[14] | (fmt[15] << 8);
      if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
    } else if (memcmp(chunk_hdr, "data", 4) == 0) {
      data_size = sz;
      have_data = true;
    } else {
      fseek(f, sz + (sz & 1), SEEK_CUR);
    }
  }
  if (!have_data || channels != 1 || bits != 16) { fclose(f); return false; }
  size_t n = data_size / 2;
  std::vector<int16_t> pcm(n);
  if (fread(pcm.data(), 2, n, f) != n) { fclose(f); return false; }
  fclose(f);
  (void)rate;  // 输入按 16k 处理（sherpa 内部可重采样，这里源即 16k）
  out.resize(n);
  for (size_t i = 0; i < n; ++i) out[i] = pcm[i] / 32768.0f;
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <in.wav> <out.f32bin>\n", argv[0]);
    return 2;
  }
  std::vector<float> wave;
  if (!read_wav16(argv[1], wave)) {
    fprintf(stderr, "wav read fail: %s\n", argv[1]);
    return 1;
  }
  sherpa_onnx::FeatureExtractorConfig cfg;  // 默认=80 维 fbank（zipformer 同款）
  sherpa_onnx::FeatureExtractor fe(cfg);
  fe.AcceptWaveform(16000, wave.data(), (int32_t)wave.size());
  fe.InputFinished();
  int32_t T = fe.NumFramesReady();
  if (T <= 0) { fprintf(stderr, "no frames\n"); return 1; }
  std::vector<float> frames = fe.GetFrames(0, T);
  int32_t dim = fe.FeatureDim();
  FILE* o = fopen(argv[2], "wb");
  if (!o) return 1;
  fwrite(&T, 4, 1, o);
  fwrite(&dim, 4, 1, o);
  fwrite(frames.data(), 4, (size_t)T * dim, o);
  fclose(o);
  printf("T=%d dim=%d -> %s\n", T, dim, argv[2]);
  return 0;
}
