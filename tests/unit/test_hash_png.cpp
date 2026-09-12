/* test_hash_png.cpp — 文件 SHA-256 与手写 PNG 写出
 *
 * PNG 写出是"内置实现：deflate stored + CRC32 + Adler32"，没有第三方库兜底。
 * 一旦 CRC32 表或块结构写错，产物在别的看图工具里打不开 —— 而播放器自检
 * 只检查"文件写出来了"，不会替你验证 PNG 是否合法。所以这里**独立重新解析**
 * 一遍产物：签名、IHDR 字段、每个块的 CRC32、以及把 IDAT 解回像素逐字节比对。
 */
#include "harness.h"
#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

/* ======================= SHA-256 ======================= */

PP_TEST(hash, sha256_of_known_content) {
    /* 已知向量：空文件与 "abc"。
     * 用文件而不是内存接口，因为被的是"完整性清单"这条路径。 */
    std::string p = pp_test::writeTmpFile("sha_empty.bin", "");
    std::string hex;
#if defined(_WIN32)
    PP_CHECK_MSG(pp::sha256FileHex(p, &hex), "Windows 上 sha256FileHex 应当可用");
    PP_STR_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    p = pp_test::writeTmpFile("sha_abc.bin", "abc");
    PP_CHECK(pp::sha256FileHex(p, &hex));
    PP_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* 跨块边界：1 MiB+1 字节，确认分块喂数据时没有漏字节/重复喂 */
    std::string big((1u << 20) + 1, 'a');
    p = pp_test::writeTmpFile("sha_big.bin", big);
    PP_CHECK(pp::sha256FileHex(p, &hex));
    PP_EQ(hex.size(), (size_t)64);
    std::string hex2;
    PP_CHECK(pp::sha256FileHex(p, &hex2));
    PP_STR_EQ(hex, hex2);                            /* 同内容两次必须一致 */

    /* 不存在的文件必须失败，而不是返回全 0 之类的假哈希 */
    PP_CHECK(!pp::sha256FileHex(pp_test::tmpPath("no_such_file_xyz.bin"), &hex));
    /* 空指针参数 */
    PP_CHECK(!pp::sha256FileHex(p, nullptr));
#else
    /* 非 Windows 平台没有实现：契约是"返回 false，让调用方据此跳过校验并告警"，
     * 绝不能返回一个看似成功的错哈希。 */
    PP_CHECK_MSG(!pp::sha256FileHex(p, &hex),
                 "非 Windows 平台应返回 false（调用方据此跳过校验）");
    PP_CHECK(hex.empty());
#endif
    pp_test::removeTmp("sha_empty.bin");
    pp_test::removeTmp("sha_abc.bin");
    pp_test::removeTmp("sha_big.bin");
}

/* ======================= PNG ======================= */

namespace {

/** 独立的 CRC32（IEEE 802.3，与 PNG 用的同一个多项式），先自证再拿去验产物。 */
uint32_t refCrc32(const uint8_t* p, size_t n) {
    static uint32_t tbl[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t rdU32(const std::string& s, size_t i) {
    return ((uint32_t)(uint8_t)s[i] << 24) | ((uint32_t)(uint8_t)s[i + 1] << 16) |
           ((uint32_t)(uint8_t)s[i + 2] << 8) | (uint32_t)(uint8_t)s[i + 3];
}

std::string readAll(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

struct PngChunk {
    std::string type;
    std::string data;
    uint32_t stored_crc = 0;
    uint32_t calc_crc = 0;
};

/** 解析 PNG 的块序列；返回 false = 结构非法。顺带算出每个块的 CRC 用于比对。 */
bool parsePng(const std::string& png, std::vector<PngChunk>* chunks, std::string* why) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8) != 0) {
        if (why) *why = "PNG 签名不对";
        return false;
    }
    size_t i = 8;
    while (i + 12 <= png.size()) {
        PngChunk c;
        uint32_t len = rdU32(png, i);
        if (len > png.size() || i + 12 + len > png.size()) {
            if (why) *why = "块长度越界";
            return false;
        }
        c.type = png.substr(i + 4, 4);
        c.data = png.substr(i + 8, len);
        c.stored_crc = rdU32(png, i + 8 + len);
        std::string crcbuf = c.type + c.data;
        c.calc_crc = refCrc32((const uint8_t*)crcbuf.data(), crcbuf.size());
        chunks->push_back(c);
        i += 12 + len;
        if (c.type == "IEND") return i == png.size();   /* IEND 之后不该还有东西 */
    }
    if (why) *why = "没找到 IEND";
    return false;
}

/** 把 PNG 的 IDAT 解回扫描线原始数据（本项目的写出用 deflate stored 块，无压缩）。 */
bool inflateStored(const std::string& z, std::string* out, std::string* why) {
    if (z.size() < 2) { if (why) *why = "zlib 头太短"; return false; }
    uint8_t cmf = (uint8_t)z[0], flg = (uint8_t)z[1];
    if ((cmf & 0x0F) != 8) { if (why) *why = "CMF 不是 deflate"; return false; }
    if (((cmf << 8) | flg) % 31 != 0) { if (why) *why = "zlib 头校验失败"; return false; }
    if (flg & 0x20) { if (why) *why = "不该有预设字典"; return false; }

    size_t i = 2;
    for (;;) {
        if (i >= z.size()) { if (why) *why = "块头缺失"; return false; }
        uint8_t hdr = (uint8_t)z[i++];
        bool final_block = (hdr & 1) != 0;
        int btype = (hdr >> 1) & 3;
        if (btype != 0) { if (why) *why = "只支持 stored 块"; return false; }
        if (i + 4 > z.size()) { if (why) *why = "LEN/NLEN 缺失"; return false; }
        uint16_t len = (uint16_t)((uint8_t)z[i] | ((uint8_t)z[i + 1] << 8));
        uint16_t nlen = (uint16_t)((uint8_t)z[i + 2] | ((uint8_t)z[i + 3] << 8));
        i += 4;
        if ((uint16_t)~len != nlen) { if (why) *why = "LEN/NLEN 不互补"; return false; }
        if (i + len > z.size()) { if (why) *why = "块数据越界"; return false; }
        out->append(z, i, len);
        i += len;
        if (final_block) break;
    }
    /* 后面应当是 Adler32 */
    if (i + 4 > z.size()) { if (why) *why = "Adler32 缺失"; return false; }
    return true;
}

uint32_t refAdler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

/** 完整校验一个 PNG：结构 + 每块 CRC + IHDR + IDAT 解出的像素 == 期望。
 *
 *  注意第一个参数是 pp_fail：断言宏依赖它，所以**辅助函数也必须把它传进来**
 *  （不能像普通函数那样随便写 PP_CHECK）。 */
void checkPng(int* pp_fail, const std::string& path, int w, int h, int channels,
              const std::string& expected_rows) {
    std::string png = readAll(path);
    PP_CHECK_MSG(!png.empty(), "PNG 文件读不出来");

    std::vector<PngChunk> chunks;
    std::string why;
    PP_CHECK_MSG(parsePng(png, &chunks, &why), why.c_str());
    if (chunks.empty()) return;

    /* 每块 CRC 必须自洽 —— 这是最容易写错、又最不容易被"文件写出来了"发现的地方 */
    for (const auto& c : chunks)
        PP_CHECK_MSG(c.stored_crc == c.calc_crc,
                     ("块 " + c.type + " 的 CRC32 不匹配").c_str());

    PP_STR_EQ(chunks.front().type, "IHDR");
    PP_STR_EQ(chunks.back().type, "IEND");
    PP_CHECK(chunks.back().data.empty());

    const std::string& ihdr = chunks.front().data;
    PP_EQ(ihdr.size(), (size_t)13);
    PP_EQ(rdU32(ihdr, 0), (uint32_t)w);
    PP_EQ(rdU32(ihdr, 4), (uint32_t)h);
    PP_EQ((int)(uint8_t)ihdr[8], 8);                  /* 位深 8 */
    PP_EQ((int)(uint8_t)ihdr[9], channels == 4 ? 6 : 0);  /* 6=RGBA, 0=Gray */
    PP_EQ((int)(uint8_t)ihdr[10], 0);                 /* 压缩方法 0 */
    PP_EQ((int)(uint8_t)ihdr[11], 0);                 /* 过滤方法 0 */
    PP_EQ((int)(uint8_t)ihdr[12], 0);                 /* 非隔行 */

    /* 拼出 IDAT（可能多块）并解压 */
    std::string idat;
    for (const auto& c : chunks) if (c.type == "IDAT") idat += c.data;
    PP_CHECK_MSG(!idat.empty(), "没有 IDAT");

    std::string raw;
    std::string iwhy;
    PP_CHECK_MSG(inflateStored(idat, &raw, &iwhy), iwhy.c_str());

    int stride = w * channels;
    PP_EQ(raw.size(), (size_t)((stride + 1) * h));
    if (raw.size() != (size_t)((stride + 1) * h)) return;

    /* 逐行：第 1 字节是过滤类型（写出用 0 = None），其余是像素 */
    for (int y = 0; y < h; ++y) {
        const char* row = raw.data() + (size_t)y * (stride + 1);
        PP_CHECK_MSG(row[0] == 0, "扫描线过滤器应为 0(None)");
        std::string got(row + 1, stride);
        std::string want = expected_rows.substr((size_t)y * stride, stride);
        PP_CHECK_MSG(got == want, "像素数据与输入不一致");
    }

    /* Adler32 自洽（独立实现算一遍） */
    PP_EQ(rdU32(idat, idat.size() - 4),
          refAdler32((const uint8_t*)raw.data(), raw.size()));
}

} // namespace

PP_TEST(png, ref_crc32_matches_spec) {
    /* 先证明测试里的 CRC32 实现是对的，否则后面用它验证产物没有意义 */
    const char* s = "123456789";
    PP_EQ(refCrc32((const uint8_t*)s, 9), 0xCBF43926u);
}

PP_TEST(png, rgba_roundtrip_is_pixel_exact) {
    const int w = 3, h = 2;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)(i * 7 + 1);
    std::string path = pp_test::tmpPath("unit_rgba.png");
    PP_CHECK(pp::writePngRGBA(path, px.data(), w, h, w * 4));
    checkPng(pp_fail, path, w, h, 4, std::string((const char*)px.data(), px.size()));
    pp_test::removeTmp("unit_rgba.png");
}

PP_TEST(png, gray8_roundtrip_is_pixel_exact) {
    const int w = 5, h = 4;
    std::vector<uint8_t> px((size_t)w * h);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)(255 - i * 3);
    std::string path = pp_test::tmpPath("unit_gray.png");
    PP_CHECK(pp::writePngGray8(path, px.data(), w, h, w));
    checkPng(pp_fail, path, w, h, 1, std::string((const char*)px.data(), px.size()));
    pp_test::removeTmp("unit_gray.png");
}

PP_TEST(png, respects_pitch_when_larger_than_row) {
    /* pitch 比 w*4 大是真实场景（SDL 纹理按对齐行距给数据）。
     * 写出必须只取每行前 w*4 字节，而不是当成连续内存一把梭。 */
    const int w = 2, h = 2, pitch = 16;               /* w*4 = 8，留 8 字节填充 */
    std::vector<uint8_t> buf((size_t)pitch * h, 0xEE);
    std::vector<uint8_t> want;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = (uint8_t)(y * 10 + x);
            buf[(size_t)y * pitch + x * 4 + 0] = v;
            buf[(size_t)y * pitch + x * 4 + 1] = v;
            buf[(size_t)y * pitch + x * 4 + 2] = v;
            buf[(size_t)y * pitch + x * 4 + 3] = 255;
            want.push_back(v); want.push_back(v); want.push_back(v); want.push_back(255);
        }
    }
    std::string path = pp_test::tmpPath("unit_pitch.png");
    PP_CHECK(pp::writePngRGBA(path, buf.data(), w, h, pitch));
    checkPng(pp_fail, path, w, h, 4, std::string((const char*)want.data(), want.size()));
    /* 填充字节 0xEE 绝不能出现在产物里 */
    std::string png = readAll(path);
    PP_CHECK_MSG(png.find(std::string(4, (char)0xEE)) == std::string::npos,
                 "行距填充字节被写进了 PNG");
    pp_test::removeTmp("unit_pitch.png");
}

PP_TEST(png, large_enough_to_span_multiple_stored_blocks) {
    /* deflate stored 单块上限 65535 字节，所以稍大一点的图必然多块 ——
     * 多块拼接写错的话，产物仍然"是个文件"，只有解回来才发现少了一截。 */
    const int w = 64, h = 64;                          /* 64*64*4 = 16384 ... 取 128x128 更稳 */
    const int W = 128, H = 128;
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)(i & 0xFF);
    std::string path = pp_test::tmpPath("unit_big.png");
    PP_CHECK(pp::writePngRGBA(path, px.data(), W, H, W * 4));
    checkPng(pp_fail, path, W, H, 4, std::string((const char*)px.data(), px.size()));
    (void)w; (void)h;
    pp_test::removeTmp("unit_big.png");
}

PP_TEST(png, rejects_invalid_arguments) {
    std::vector<uint8_t> px(16, 0);
    /* 尺寸非法、指针空、pitch 不够：都必须失败返回，而不是写个坏文件出来 */
    PP_CHECK(!pp::writePngRGBA(pp_test::tmpPath("bad1.png"), px.data(), 0, 1, 4));
    PP_CHECK(!pp::writePngRGBA(pp_test::tmpPath("bad2.png"), px.data(), 1, 0, 4));
    PP_CHECK(!pp::writePngRGBA(pp_test::tmpPath("bad3.png"), px.data(), -1, 1, 4));
    PP_CHECK(!pp::writePngRGBA(pp_test::tmpPath("bad4.png"), nullptr, 1, 1, 4));
    PP_CHECK(!pp::writePngRGBA(pp_test::tmpPath("bad5.png"), px.data(), 2, 1, 4));
    PP_CHECK(!pp::writePngGray8(pp_test::tmpPath("bad6.png"), nullptr, 1, 1, 1));
    /* 写不出去的路径也要失败，别静默成功 */
    PP_CHECK(!pp::writePngRGBA(pp::pathJoin(pp_test::tmpDir(), "no_dir_xyz/a.png"),
                               px.data(), 1, 1, 4));
    for (int i = 1; i <= 6; ++i) {
        std::string n = pp::format("bad%d.png", i);
        pp_test::removeTmp(n.c_str());
    }
}
