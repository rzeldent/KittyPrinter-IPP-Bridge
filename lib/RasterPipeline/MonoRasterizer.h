/**
 * @file MonoRasterizer.h
 * @brief Streaming grayscale → 1-bit conversion for thermal printing.
 *
 * Pipeline (one source line at a time, never a whole page in memory):
 * @verbatim
 *   8-bit gray line (any width)
 *        │  horizontal resample (box filter when shrinking,
 *        │  nearest neighbour when enlarging) to the print width (384 px)
 *        ▼
 *   vertical resample with the same factor (aspect ratio preserved)
 *        ▼
 *   serpentine Atkinson error diffusion (Floyd–Steinberg selectable at build time)
 *        ▼
 *   1-bit packed row (MSB first, 1 = black) → RowFn callback
 * @endverbatim
 *
 * Memory use is O(output width): three error rows (two for Floyd–Steinberg)
 * and one accumulator row.
 * Optionally removes blank rows at the top and bottom of every page to save
 * paper (very effective for A4/Letter documents printed on a 58 mm roll).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#ifndef KITTY_IPP_BRIDGE_DITHER_ATKINSON
#define KITTY_IPP_BRIDGE_DITHER_ATKINSON 1
#endif

#if KITTY_IPP_BRIDGE_DITHER_ATKINSON != 0 && KITTY_IPP_BRIDGE_DITHER_ATKINSON != 1
#error "KITTY_IPP_BRIDGE_DITHER_ATKINSON must be 0 or 1"
#endif

namespace raster
{

    class MonoRasterizer
    {
    public:
        using RowFn = std::function<void(const uint8_t *row, size_t rowBytes)>;

        struct Options
        {
            uint16_t outWidth = 384;    ///< Printer width in pixels
            bool trimBlankRows = true;  ///< Drop leading/trailing white rows of each page
            bool dither = true;         ///< Build-selected error diffusion (false = simple threshold)
            uint8_t threshold = 128;    ///< Black/white decision level
        };

        /** Start a new page of @p srcWidth x @p srcHeight gray pixels. */
        void beginPage(uint32_t srcWidth, uint32_t srcHeight, const Options &options, RowFn sink);

        /** Push the next source line (srcWidth bytes, 0 = black, 255 = white). */
        void pushLine(const uint8_t *gray);

        /** Flush the page. Trailing blank rows are discarded when trimming. */
        void endPage();

        /** Rows delivered to the sink for the current page. */
        uint32_t rowsEmitted() const { return emitted_; }

        /** Output height the page will have before trimming. */
        uint32_t scaledHeight() const { return dstHeight_; }

    private:
        void emitScaledRow();
        void ditherAndOutput(const uint16_t *row);
        void deliver(const uint8_t *packed, bool blank);

        Options opt_;
        RowFn sink_;
        uint32_t srcWidth_ = 0;
        uint32_t srcHeight_ = 0;
        uint32_t dstHeight_ = 0;
        uint32_t srcY_ = 0;
        uint32_t dstY_ = 0;
        uint32_t emitted_ = 0;
        uint32_t accLines_ = 0;
        uint32_t pendingBlank_ = 0;
        bool seenInk_ = false;
        std::vector<uint32_t> acc_;     ///< Vertical accumulator (sum of horizontally scaled values)
        std::vector<uint32_t> hsum_;    ///< Scratch for horizontal box filter
        std::vector<uint16_t> hcount_;  ///< Pixels contributing to each output column
        std::vector<uint16_t> scaled_;  ///< Current output row before dithering
        std::vector<int16_t> errCur_;   ///< Error diffusion, current row (two guard pixels per side)
        std::vector<int16_t> errNext_;  ///< Error diffusion, next row
#if KITTY_IPP_BRIDGE_DITHER_ATKINSON
        std::vector<int16_t> errAfterNext_; ///< Atkinson also diffuses two rows ahead
#endif
        std::vector<uint8_t> packed_;   ///< Packed output row
    };

} // namespace raster
