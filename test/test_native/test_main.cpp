/**
 * Host unit tests for the hardware independent libraries:
 *   - CatProtocol    (packet framing, CRC, bit order, flow control)
 *   - IppProtocol    (request parsing, response encoding)
 *   - RasterPipeline (PWG decoding, scaling/dithering, text rendering)
 *
 * Run with: pio test -e native
 */
#include <unity.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "CatProtocol.h"
#include "IppMessage.h"
#include "MonoBitmap.h"
#include "MonoRasterizer.h"
#include "PwgJobConverter.h"
#include "PwgRasterDecoder.h"
#include "TextRenderer.h"

static bool failNextAllocation = false;

void *operator new(size_t size)
{
    if (failNextAllocation)
    {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

static std::string hex(const std::vector<uint8_t> &v)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (auto b : v)
    {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

// ---------------------------------------------------------------------------
// CatProtocol
// ---------------------------------------------------------------------------
static void test_crc8_known_packets()
{
    std::vector<uint8_t> out;
    const uint8_t quality = 0x33;
    catgfx::LegacyEncoder::packet(out, catgfx::LegacyEncoder::CMD_QUALITY, &quality, 1);
    TEST_ASSERT_EQUAL_STRING("5178a40001003399ff", hex(out).c_str());

    out.clear();
    const uint8_t zero = 0;
    catgfx::LegacyEncoder::packet(out, catgfx::LegacyEncoder::CMD_GET_STATE, &zero, 1);
    TEST_ASSERT_EQUAL_STRING("5178a30001000000ff", hex(out).c_str());
}

static void test_lattice_start_packet()
{
    catgfx::LegacyEncoder enc;
    std::vector<uint8_t> out;
    catgfx::PrintSettings s;
    enc.beginJob(out, s);
    const auto h = hex(out);
    TEST_ASSERT_TRUE(h.find("5178a6000b00aa551738445f5f5f44382ca1ff") != std::string::npos);
}

static void test_bitmap_row_bit_order()
{
    catgfx::LegacyEncoder enc(384);
    std::vector<uint8_t> row(48, 0);
    row[0] = 0x80;  // left-most pixel black (MSB first)
    row[47] = 0x01; // right-most pixel black
    std::vector<uint8_t> out;
    enc.bitmapRow(out, row.data(), row.size());
    TEST_ASSERT_EQUAL(48 + 8, out.size());
    TEST_ASSERT_EQUAL_HEX8(0xA2, out[2]);
    TEST_ASSERT_EQUAL_HEX8(48, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[6]); // LSB first on the wire
    TEST_ASSERT_EQUAL_HEX8(0x80, out[6 + 47]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out.back());
}

static void test_feed_splits_large_values()
{
    catgfx::LegacyEncoder enc;
    std::vector<uint8_t> out;
    enc.feed(out, 300);
    // 255 + 45 lines -> two packets of 10 bytes
    TEST_ASSERT_EQUAL(20, out.size());
    TEST_ASSERT_EQUAL_HEX8(0xA1, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[6]);
    TEST_ASSERT_EQUAL_HEX8(45, out[16]);
}

static void test_flow_control()
{
    catgfx::LegacyEncoder enc;
    const uint8_t pause[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x10, 0x70, 0xFF};
    const uint8_t resume[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x00, 0x00, 0xFF};
    const uint8_t other[] = {0x51, 0x78, 0xA3, 0x01, 0x03, 0x00, 0x00, 0x0C, 0x10, 0x00, 0xFF};
    TEST_ASSERT_TRUE(enc.parseNotification(pause, sizeof(pause)) == catgfx::FlowControl::Pause);
    TEST_ASSERT_TRUE(enc.parseNotification(resume, sizeof(resume)) == catgfx::FlowControl::Resume);
    TEST_ASSERT_TRUE(enc.parseNotification(other, sizeof(other)) == catgfx::FlowControl::None);
}

static void test_encoder_factory()
{
    TEST_ASSERT_NOT_NULL(catgfx::createEncoder(catgfx::ProtocolFamily::CatGfxLegacy).get());
    TEST_ASSERT_NULL(catgfx::createEncoder(catgfx::ProtocolFamily::CatGfxV2).get());
}

static void test_flow_control_combined_notifications()
{
    catgfx::LegacyEncoder enc;
    const uint8_t pause[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x10, 0x70, 0xFF};
    const uint8_t resume[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x00, 0x00, 0xFF};
    const uint8_t status[] = {0x51, 0x78, 0xA3, 0x01, 0x01, 0x00, 0x00, 0x00, 0xFF};
    std::vector<uint8_t> notification(pause, pause + sizeof(pause));
    notification.insert(notification.end(), status, status + sizeof(status));
    notification.insert(notification.end(), resume, resume + sizeof(resume));
    TEST_ASSERT_TRUE(enc.parseNotification(notification.data(), notification.size()) == catgfx::FlowControl::Resume);
    notification.insert(notification.end(), pause, pause + sizeof(pause));
    TEST_ASSERT_TRUE(enc.parseNotification(notification.data(), notification.size()) == catgfx::FlowControl::Pause);
}

static void test_flow_control_rejects_invalid_notifications()
{
    catgfx::LegacyEncoder enc;
    uint8_t frame[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x10, 0x70, 0xFF};
    TEST_ASSERT_TRUE(enc.parseNotification(frame, sizeof(frame) - 1) == catgfx::FlowControl::None);
    frame[7] ^= 1;
    TEST_ASSERT_TRUE(enc.parseNotification(frame, sizeof(frame)) == catgfx::FlowControl::None);
    frame[6] = 0x01;
    frame[7] = catgfx::crc8(frame + 6, 1);
    TEST_ASSERT_TRUE(enc.parseNotification(frame, sizeof(frame)) == catgfx::FlowControl::None);
    frame[6] = 0x10;
    frame[7] = 0x70;
    frame[8] = 0;
    TEST_ASSERT_TRUE(enc.parseNotification(frame, sizeof(frame)) == catgfx::FlowControl::None);
}

// ---------------------------------------------------------------------------
// IppProtocol
// ---------------------------------------------------------------------------
static std::vector<uint8_t> buildPrintJobRequest()
{
    ipp::Writer w;
    w.header(2, 0, ipp::OP_PRINT_JOB, 42);
    w.group(ipp::TAG_OPERATION);
    w.charset("attributes-charset", "utf-8");
    w.language("attributes-natural-language", "en");
    w.uri("printer-uri", "ipp://kitty-ipp-bridge.local/ipp/aabbccddeeff");
    w.name("requesting-user-name", "alice");
    w.mimeType("document-format", "application/pwg-raster");
    w.group(ipp::TAG_JOB);
    w.beginCollection("media-col");
    w.memberName("media-size");
    w.beginCollection("");
    w.memberName("x-dimension");
    w.integer("", 5800);
    w.endCollection();
    w.endCollection();
    w.integer("copies", 1);
    w.keywords("job-sheets", {"none", "none"});
    w.end();
    return w.data();
}

static void test_ipp_parse_complete_request()
{
    auto msg = buildPrintJobRequest();
    const auto attrLen = msg.size();
    const char doc[] = "RaS2-document";
    msg.insert(msg.end(), doc, doc + sizeof(doc) - 1);

    ipp::RequestParser p;
    const auto consumed = p.feed(msg.data(), msg.size());
    TEST_ASSERT_TRUE(p.complete());
    TEST_ASSERT_FALSE(p.error());
    TEST_ASSERT_EQUAL(attrLen, consumed);

    const ipp::Request &r = p.request();
    TEST_ASSERT_EQUAL(2, r.versionMajor);
    TEST_ASSERT_EQUAL_HEX16(ipp::OP_PRINT_JOB, r.operation);
    TEST_ASSERT_EQUAL(42, r.requestId);
    TEST_ASSERT_EQUAL_STRING("application/pwg-raster", r.getString("document-format").c_str());
    TEST_ASSERT_EQUAL_STRING("alice", r.getString("requesting-user-name").c_str());
    TEST_ASSERT_EQUAL(1, r.getInt("copies"));
    TEST_ASSERT_EQUAL(2, r.getStrings("job-sheets").size());
    // Collection members must not leak as top level attributes
    TEST_ASSERT_NULL(r.find("x-dimension"));
    TEST_ASSERT_NOT_NULL(r.find("media-col"));
}

static void test_ipp_parse_byte_by_byte()
{
    auto msg = buildPrintJobRequest();
    const auto attrLen = msg.size();
    msg.push_back('X');
    ipp::RequestParser p;
    size_t total = 0;
    size_t i = 0;
    for (; i < msg.size() && !p.complete(); ++i)
        total += p.feed(&msg[i], 1);
    TEST_ASSERT_TRUE(p.complete());
    TEST_ASSERT_EQUAL(attrLen, total);
    TEST_ASSERT_EQUAL(attrLen, i);
}

static void test_ipp_parse_rejects_garbage()
{
    // Attribute before any group delimiter
    std::vector<uint8_t> msg = {2, 0, 0, 0x0B, 0, 0, 0, 1, 0x47, 0, 1, 'a', 0, 1, 'b', 3};
    ipp::RequestParser p;
    p.feed(msg.data(), msg.size());
    TEST_ASSERT_TRUE(p.error());

    // Attribute section larger than the configured limit
    ipp::RequestParser limited(32);
    std::vector<uint8_t> big(64, 0x01);
    big[0] = 2;
    big[1] = 0;
    limited.feed(big.data(), big.size());
    TEST_ASSERT_TRUE(limited.error());
}

static void test_ipp_writer_encoding()
{
    ipp::Writer w;
    w.header(1, 1, ipp::STATUS_OK, 7);
    w.group(ipp::TAG_PRINTER);
    w.integer("queued-job-count", 3);
    w.end();
    TEST_ASSERT_EQUAL_STRING("0101000000000007"
                             "04"
                             "21"
                             "0010"
                             "717565756564"         // "queued"
                             "2d6a6f622d636f756e74" // "-job-count"
                             "0004"
                             "00000003"
                             "03",
                             hex(w.data()).c_str());
}

// ---------------------------------------------------------------------------
// RasterPipeline
// ---------------------------------------------------------------------------
static void put32(std::vector<uint8_t> &h, size_t off, uint32_t v)
{
    h[off] = static_cast<uint8_t>(v >> 24);
    h[off + 1] = static_cast<uint8_t>(v >> 16);
    h[off + 2] = static_cast<uint8_t>(v >> 8);
    h[off + 3] = static_cast<uint8_t>(v);
}

static std::vector<uint8_t> pwgHeader(uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpp, uint32_t cs)
{
    std::vector<uint8_t> hdr(1796, 0);
    memcpy(hdr.data(), "PwgRaster", 9);
    put32(hdr, 276, 203);
    put32(hdr, 280, 203);
    put32(hdr, 372, w);
    put32(hdr, 376, h);
    put32(hdr, 384, bpc);
    put32(hdr, 388, bpp);
    put32(hdr, 392, (w * bpp + 7) / 8);
    put32(hdr, 400, cs);
    put32(hdr, 420, bpp / bpc);
    return hdr;
}

static void test_pwg_decode_sgray_compressed()
{
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    auto hdr = pwgHeader(8, 4, 8, 8, raster::CSPACE_SW);
    s.insert(s.end(), hdr.begin(), hdr.end());
    // line 0 (+1 repeat): 4 x 0x00, 4 x 0xFF
    s.insert(s.end(), {0x01, 0x03, 0x00, 0x03, 0xFF});
    // line 2: 8 literal pixels (257 - 249 = 8)
    s.insert(s.end(), {0x00, 249, 0, 32, 64, 96, 128, 160, 192, 224});
    // line 3: all white
    s.insert(s.end(), {0x00, 0x07, 0xFF});

    raster::PwgRasterDecoder dec;
    std::vector<std::vector<uint8_t>> lines;
    raster::PwgPageHeader seen;
    dec.onPageStart([&](const raster::PwgPageHeader &h, uint32_t) {
        seen = h;
        return raster::isSupported(h);
    });
    dec.onLine([&](const uint8_t *line, uint32_t) { lines.emplace_back(line, line + 8); });

    // Feed in awkward chunk sizes to exercise the state machine.
    for (size_t i = 0; i < s.size(); i += 7)
        TEST_ASSERT_TRUE(dec.feed(s.data() + i, std::min<size_t>(7, s.size() - i)));
    TEST_ASSERT_TRUE(dec.finish());
    TEST_ASSERT_EQUAL(1, dec.pagesCompleted());
    TEST_ASSERT_EQUAL(8, seen.width);
    TEST_ASSERT_EQUAL(4, lines.size());
    const uint8_t l0[] = {0, 0, 0, 0, 255, 255, 255, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[0].data(), 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[1].data(), 8);
    TEST_ASSERT_EQUAL_UINT8(224, lines[2][7]);
    TEST_ASSERT_EQUAL_UINT8(255, lines[3][0]);
}

static void test_pwg_decode_multi_page_and_rgb()
{
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    for (int page = 0; page < 2; ++page)
    {
        auto hdr = pwgHeader(2, 1, 8, 24, raster::CSPACE_SRGB);
        s.insert(s.end(), hdr.begin(), hdr.end());
        // one line: 2 literal RGB pixels: red, white
        s.insert(s.end(), {0x00, 255, 255, 0, 0, 255, 255, 255});
    }
    raster::PwgRasterDecoder dec;
    raster::PwgPageHeader hdr;
    std::vector<uint8_t> gray;
    dec.onPageStart([&](const raster::PwgPageHeader &h, uint32_t) {
        hdr = h;
        return raster::isSupported(h);
    });
    dec.onLine([&](const uint8_t *line, uint32_t) {
        uint8_t g[2];
        raster::toGray(hdr, line, g);
        gray.push_back(g[0]);
        gray.push_back(g[1]);
    });
    TEST_ASSERT_TRUE(dec.feed(s.data(), s.size()));
    TEST_ASSERT_TRUE(dec.finish());
    TEST_ASSERT_EQUAL(2, dec.pagesCompleted());
    TEST_ASSERT_EQUAL(4, gray.size());
    TEST_ASSERT_UINT8_WITHIN(2, 76, gray[0]); // luma of pure red
    TEST_ASSERT_EQUAL_UINT8(255, gray[1]);
}

static void test_urf_decode_sgray_fill_white()
{
    std::vector<uint8_t> s = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0, 0, 0, 0, 1};
    std::vector<uint8_t> hdr(32, 0);
    hdr[0] = 8; // bits per pixel
    hdr[1] = 0; // sGray
    put32(hdr, 12, 8);
    put32(hdr, 16, 3);
    put32(hdr, 20, 300);
    s.insert(s.end(), hdr.begin(), hdr.end());
    // line 0 (+1 repeat): 2 x 0x00, then fill the rest with white
    s.insert(s.end(), {0x01, 0x01, 0x00, 0x80});
    // line 2: 2 literal pixels, then fill
    s.insert(s.end(), {0x00, 255, 10, 20, 0x80});

    raster::PwgRasterDecoder dec;
    std::vector<std::vector<uint8_t>> lines;
    raster::PwgPageHeader seen;
    dec.onPageStart([&](const raster::PwgPageHeader &h, uint32_t) {
        seen = h;
        return raster::isSupported(h);
    });
    dec.onLine([&](const uint8_t *line, uint32_t) { lines.emplace_back(line, line + 8); });

    for (size_t i = 0; i < s.size(); i += 5)
        TEST_ASSERT_TRUE(dec.feed(s.data() + i, std::min<size_t>(5, s.size() - i)));
    TEST_ASSERT_TRUE(dec.finish());
    TEST_ASSERT_TRUE(dec.format() == raster::PwgRasterDecoder::Format::Urf);
    TEST_ASSERT_EQUAL(1, dec.pagesCompleted());
    TEST_ASSERT_EQUAL(raster::CSPACE_SW, seen.colorSpace);
    TEST_ASSERT_EQUAL(300, seen.hwResolutionX);
    TEST_ASSERT_EQUAL(3, lines.size());
    const uint8_t l0[] = {0, 0, 255, 255, 255, 255, 255, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[0].data(), 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[1].data(), 8);
    const uint8_t l2[] = {10, 20, 255, 255, 255, 255, 255, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l2, lines[2].data(), 8);
}

static void test_pwg_rejects_bad_streams()
{
    raster::PwgRasterDecoder dec;
    const uint8_t bad[] = {'%', 'P', 'D', 'F'};
    TEST_ASSERT_FALSE(dec.feed(bad, sizeof(bad)));
    TEST_ASSERT_TRUE(dec.failed());

    // Truncated page
    raster::PwgRasterDecoder dec2;
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    auto hdr = pwgHeader(8, 2, 8, 8, raster::CSPACE_SW);
    s.insert(s.end(), hdr.begin(), hdr.end());
    s.insert(s.end(), {0x00, 0x07, 0xFF});
    TEST_ASSERT_TRUE(dec2.feed(s.data(), s.size()));
    TEST_ASSERT_FALSE(dec2.finish());

    // Unsupported colour space (CMYK = 6)
    raster::PwgPageHeader h;
    h.width = 1;
    h.height = 1;
    h.bitsPerColor = 8;
    h.bitsPerPixel = 32;
    h.bytesPerLine = 4;
    h.colorSpace = 6;
    TEST_ASSERT_FALSE(raster::isSupported(h));
}

static void test_rasterizer_downscale_black()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    std::vector<std::vector<uint8_t>> rows;
    r.beginPage(768, 100, opt, [&](const uint8_t *row, size_t n) { rows.emplace_back(row, row + n); });
    std::vector<uint8_t> black(768, 0);
    for (int y = 0; y < 100; ++y)
        r.pushLine(black.data());
    r.endPage();
    TEST_ASSERT_EQUAL(50, rows.size());
    for (const auto &row : rows)
    {
        TEST_ASSERT_EQUAL(48, row.size());
        for (auto b : row)
            TEST_ASSERT_EQUAL_HEX8(0xFF, b);
    }
}

static void test_rasterizer_trims_blank_rows()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    uint32_t count = 0;
    r.beginPage(384, 30, opt, [&](const uint8_t *, size_t) { ++count; });
    std::vector<uint8_t> white(384, 255), black(384, 0);
    for (int y = 0; y < 30; ++y)
        r.pushLine((y == 10 || y == 12) ? black.data() : white.data());
    r.endPage();
    // Leading 10 and trailing 17 rows removed, interior blank row kept.
    TEST_ASSERT_EQUAL(3, count);

    opt.trimBlankRows = false;
    count = 0;
    r.beginPage(384, 30, opt, [&](const uint8_t *, size_t) { ++count; });
    for (int y = 0; y < 30; ++y)
        r.pushLine(white.data());
    r.endPage();
    TEST_ASSERT_EQUAL(30, count);
}

static void test_rasterizer_dither_midgray()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    opt.trimBlankRows = false;
    uint32_t black = 0, total = 0;
    r.beginPage(192, 50, opt, [&](const uint8_t *row, size_t n) {
        for (size_t i = 0; i < n; ++i)
            for (int b = 0; b < 8; ++b)
            {
                black += (row[i] >> b) & 1;
                ++total;
            }
    });
    std::vector<uint8_t> gray(192, 128);
    for (int y = 0; y < 50; ++y)
        r.pushLine(gray.data());
    r.endPage();
    TEST_ASSERT_EQUAL(100 * 384, total); // upscaled 2x in both directions
    const double ratio = static_cast<double>(black) / total;
    TEST_ASSERT_TRUE(ratio > 0.40 && ratio < 0.60);
}


static void test_rasterizer_dither_reference_and_reset()
{
    const uint8_t gray[][8] = {
        {32, 64, 96, 128, 160, 192, 224, 240},
        {240, 224, 192, 160, 128, 96, 64, 32},
        {100, 100, 100, 100, 100, 100, 100, 100},
        {140, 140, 140, 140, 140, 140, 140, 140},
    };
#if KITTY_IPP_BRIDGE_DITHER_ATKINSON
    const uint8_t expected[] = {0xE0, 0x0F, 0xF6, 0x10};
#else
    const uint8_t expected[] = {0xD0, 0x0F, 0xF5, 0x0A};
#endif
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    opt.outWidth = 8;
    opt.trimBlankRows = false;
    for (auto page = 0; page < 2; ++page)
    {
        std::vector<uint8_t> output;
        r.beginPage(8, 4, opt, [&](const uint8_t *row, size_t n) { output.insert(output.end(), row, row + n); });
        for (const auto &row : gray)
            r.pushLine(row);
        r.endPage();
        TEST_ASSERT_EQUAL(sizeof(expected), output.size());
        TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, output.data(), sizeof(expected));
    }
}

static void test_rasterizer_dither_narrow_and_threshold()
{
    const uint8_t gray[] = {100, 0, 118};
#if KITTY_IPP_BRIDGE_DITHER_ATKINSON
    const uint8_t expected[] = {0x80, 0x80, 0x00};
#else
    const uint8_t expected[] = {0x80, 0x80, 0x80};
#endif
    const uint8_t threshold[] = {0x80, 0x80, 0x80};
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    opt.outWidth = 1;
    opt.trimBlankRows = false;
    for (const auto dither : {true, false})
    {
        opt.dither = dither;
        std::vector<uint8_t> output;
        r.beginPage(1, 3, opt, [&](const uint8_t *row, size_t n) { output.insert(output.end(), row, row + n); });
        for (const auto &pixel : gray)
            r.pushLine(&pixel);
        r.endPage();
        TEST_ASSERT_EQUAL(sizeof(expected), output.size());
        TEST_ASSERT_EQUAL_HEX8_ARRAY(dither ? expected : threshold, output.data(), sizeof(expected));
    }
}

static std::vector<uint8_t> blackPages(uint32_t pages, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    for (uint32_t p = 0; p < pages; ++p)
    {
        auto hdr = pwgHeader(w, h, 8, 8, raster::CSPACE_SW);
        s.insert(s.end(), hdr.begin(), hdr.end());
        // first line + (h-1) repeats, all pixels black (w <= 128*... use runs of 128)
        s.push_back(static_cast<uint8_t>(h - 1));
        for (uint32_t x = 0; x < w; x += 128)
        {
            const auto n = std::min<uint32_t>(128, w - x);
            s.push_back(static_cast<uint8_t>(n - 1));
            s.push_back(0x00);
        }
    }
    return s;
}

static void test_converter_multi_page()
{
    raster::PwgJobConverter::Options opt;
    raster::PwgJobConverter conv(opt);
    auto doc = blackPages(2, 768, 40);
    for (size_t i = 0; i < doc.size(); i += 100)
        TEST_ASSERT_TRUE(conv.feed(doc.data() + i, std::min<size_t>(100, doc.size() - i)));
    TEST_ASSERT_TRUE(conv.finish());
    TEST_ASSERT_EQUAL(2, conv.pages());
    TEST_ASSERT_EQUAL(20 + opt.pageGapRows + 20, conv.bitmap().height);
    TEST_ASSERT_EQUAL_HEX8(0xFF, conv.bitmap().row(0)[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, conv.bitmap().row(25)[0]);
}

static void test_converter_budget_and_errors()
{
    raster::PwgJobConverter::Options opt;
    opt.maxOutputBytes = 48 * 10;
    raster::PwgJobConverter conv(opt);
    auto doc = blackPages(1, 384, 100);
    TEST_ASSERT_FALSE(conv.feed(doc.data(), doc.size()));
    TEST_ASSERT_TRUE(conv.overBudget());
    TEST_ASSERT_FALSE(conv.finish());

    raster::PwgJobConverter empty(raster::PwgJobConverter::Options{});
    const uint8_t sync[] = {'R', 'a', 'S', '2'};
    TEST_ASSERT_TRUE(empty.feed(sync, sizeof(sync)));
    TEST_ASSERT_FALSE(empty.finish());
    TEST_ASSERT_FALSE(empty.error().empty());
}

static void test_converter_exact_budget_and_capacity()
{
    raster::PwgJobConverter::Options opt;
    opt.maxOutputBytes = 48 * 10 + 1;
    raster::PwgJobConverter conv(opt);
    auto doc = blackPages(1, 384, 10);
    TEST_ASSERT_TRUE(conv.feed(doc.data(), doc.size()));
    TEST_ASSERT_TRUE(conv.finish());
    TEST_ASSERT_EQUAL(10, conv.bitmap().height);
    TEST_ASSERT_EQUAL(480, conv.bitmap().data.size());
    TEST_ASSERT_TRUE(conv.bitmap().data.capacity() <= opt.maxOutputBytes);

    raster::PwgJobConverter larger(opt);
    doc = blackPages(1, 384, 11);
    TEST_ASSERT_FALSE(larger.feed(doc.data(), doc.size()));
    TEST_ASSERT_TRUE(larger.overBudget());
    TEST_ASSERT_EQUAL(480, larger.bitmap().data.size());
    TEST_ASSERT_TRUE(larger.bitmap().data.capacity() <= opt.maxOutputBytes);
}

static void test_converter_page_gap_budget()
{
    raster::PwgJobConverter::Options opt;
    opt.pageGapRows = 3;
    opt.maxOutputBytes = 48 * 5;
    auto doc = blackPages(2, 384, 1);
    raster::PwgJobConverter exact(opt);
    TEST_ASSERT_TRUE(exact.feed(doc.data(), doc.size()));
    TEST_ASSERT_TRUE(exact.finish());
    TEST_ASSERT_EQUAL(5, exact.bitmap().height);
    TEST_ASSERT_EQUAL_HEX8(0, exact.bitmap().row(1)[0]);

    opt.pageGapRows = 5;
    raster::PwgJobConverter over(opt);
    TEST_ASSERT_FALSE(over.feed(doc.data(), doc.size()));
    TEST_ASSERT_TRUE(over.overBudget());
    TEST_ASSERT_EQUAL(1, over.bitmap().height);
    TEST_ASSERT_EQUAL(48, over.bitmap().data.size());
    TEST_ASSERT_FALSE(over.finish());
    TEST_ASSERT_EQUAL_STRING("document exceeds memory budget", over.error().c_str());
}

static void test_converter_allocation_failure()
{
    auto doc = blackPages(1, 384, 10);
    const auto headerBytes = 4 + pwgHeader(384, 10, 8, 8, raster::CSPACE_SW).size();
    // Fail either the source-line allocation or the first output-buffer allocation.
    for (const auto prefix : {size_t{4}, headerBytes})
    {
        raster::PwgJobConverter conv(raster::PwgJobConverter::Options{});
        TEST_ASSERT_TRUE(conv.feed(doc.data(), prefix));
        failNextAllocation = true;
        const auto ok = conv.feed(doc.data() + prefix, doc.size() - prefix);
        const auto injected = !failNextAllocation;
        failNextAllocation = false;
        TEST_ASSERT_TRUE(injected);
        TEST_ASSERT_FALSE(ok);
        TEST_ASSERT_TRUE(conv.failed());
        TEST_ASSERT_TRUE(conv.outOfMemory());
        TEST_ASSERT_FALSE(conv.overBudget());
        TEST_ASSERT_EQUAL_STRING("out of memory", conv.error().c_str());
        TEST_ASSERT_FALSE(conv.finish());
        TEST_ASSERT_FALSE(conv.feed(doc.data(), doc.size()));
    }

    // Keep one row in the buffer, then fail its growth while decoding repeated rows.
    doc[headerBytes] = 0;
    raster::PwgJobConverter growing(raster::PwgJobConverter::Options{});
    TEST_ASSERT_TRUE(growing.feed(doc.data(), doc.size()));
    TEST_ASSERT_EQUAL(1, growing.bitmap().height);
    const uint8_t remaining[] = {8, 127, 0, 127, 0, 127, 0};
    failNextAllocation = true;
    const auto ok = growing.feed(remaining, sizeof(remaining));
    const auto injected = !failNextAllocation;
    failNextAllocation = false;
    TEST_ASSERT_TRUE(injected);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(growing.outOfMemory());
    TEST_ASSERT_EQUAL(1, growing.bitmap().height);
    TEST_ASSERT_EQUAL(48, growing.bitmap().data.size());
    TEST_ASSERT_EQUAL_HEX8(0xFF, growing.bitmap().row(0)[0]);
}

static void test_identify_page()
{
    auto bmp = raster::buildIdentifyPage("Desk Label Printer", "aa:bb:cc:dd:ee:ff");
    TEST_ASSERT_EQUAL(384, bmp.width);
    TEST_ASSERT_TRUE(bmp.height > 60);
    TEST_ASSERT_EQUAL(bmp.height * 48, bmp.data.size());
    size_t ink = 0;
    for (auto b : bmp.data)
        ink += b ? 1 : 0;
    TEST_ASSERT_TRUE(ink > 100);

    auto test = raster::buildTestPage({"line one", "line two"});
    TEST_ASSERT_TRUE(test.height > 0);
    TEST_ASSERT_EQUAL(test.height * 48, test.data.size());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8_known_packets);
    RUN_TEST(test_lattice_start_packet);
    RUN_TEST(test_bitmap_row_bit_order);
    RUN_TEST(test_feed_splits_large_values);
    RUN_TEST(test_flow_control);
    RUN_TEST(test_flow_control_combined_notifications);
    RUN_TEST(test_flow_control_rejects_invalid_notifications);
    RUN_TEST(test_encoder_factory);
    RUN_TEST(test_ipp_parse_complete_request);
    RUN_TEST(test_ipp_parse_byte_by_byte);
    RUN_TEST(test_ipp_parse_rejects_garbage);
    RUN_TEST(test_ipp_writer_encoding);
    RUN_TEST(test_pwg_decode_sgray_compressed);
    RUN_TEST(test_pwg_decode_multi_page_and_rgb);
    RUN_TEST(test_urf_decode_sgray_fill_white);
    RUN_TEST(test_pwg_rejects_bad_streams);
    RUN_TEST(test_rasterizer_downscale_black);
    RUN_TEST(test_rasterizer_trims_blank_rows);
    RUN_TEST(test_rasterizer_dither_midgray);
    RUN_TEST(test_rasterizer_dither_reference_and_reset);
    RUN_TEST(test_rasterizer_dither_narrow_and_threshold);
    RUN_TEST(test_converter_multi_page);
    RUN_TEST(test_converter_budget_and_errors);
    RUN_TEST(test_converter_exact_budget_and_capacity);
    RUN_TEST(test_converter_page_gap_budget);
    RUN_TEST(test_converter_allocation_failure);
    RUN_TEST(test_identify_page);
    return UNITY_END();
}
