/**
 * @file PwgJobConverter.cpp
 * @brief Streaming conversion of a PWG raster document into a printer bitmap.
 */
#include "PwgJobConverter.h"

#include <algorithm>
#include <limits>
#include <new>

namespace raster
{

    PwgJobConverter::PwgJobConverter(const Options &options) : opt_(options), bitmap_(options.width ? options.width : 384)
    {
        // Printer pages are narrow; refuse absurd dimensions early (1200 dpi A3 fits).
        decoder_.setLimits(16000, 60000);

        decoder_.onPageStart([this](const PwgPageHeader &h, uint32_t index) {
            if (failed())
                return false;
            std::string reason;
            if (!isSupported(h, &reason))
            {
                error_ = "unsupported raster: " + reason;
                return false;
            }
            if (index >= opt_.maxPages)
            {
                error_ = "too many pages";
                return false;
            }
            header_ = h;
            gray_.assign(h.width, 0xFF);
            if (index > 0 && bitmap_.height > 0)
            {
                if (!ensureRows(opt_.pageGapRows))
                    return false;
                bitmap_.appendBlank(opt_.pageGapRows);
            }

            MonoRasterizer::Options ro;
            ro.outWidth = bitmap_.width;
            ro.trimBlankRows = opt_.trimBlankRows;
            ro.dither = opt_.dither;
            rasterizer_.beginPage(h.width, h.height, ro, [this](const uint8_t *row, size_t) {
                if (failed() || !ensureRows(1))
                    return;
                bitmap_.appendRow(row);
            });
            return true;
        });

        decoder_.onLine([this](const uint8_t *line, uint32_t) {
            if (failed())
                return;
            toGray(header_, line, gray_.data());
            rasterizer_.pushLine(gray_.data());
        });

        decoder_.onPageEnd([this](uint32_t) {
            if (!failed())
                rasterizer_.endPage();
        });
    }

    bool PwgJobConverter::ensureRows(uint32_t rows)
    {
        const auto rowBytes = bitmap_.rowBytes();
        const auto maxBytes = opt_.maxOutputBytes;
        const auto size = bitmap_.data.size();
        if (size > maxBytes || rows > (maxBytes - size) / rowBytes ||
            rows > std::numeric_limits<uint32_t>::max() - bitmap_.height)
        {
            overBudget_ = true;
            error_ = "document exceeds memory budget";
            return false;
        }
        const auto required = size + static_cast<size_t>(rows) * rowBytes;
        const auto capacity = bitmap_.data.capacity();
        if (required > capacity)
        {
            // Explicit growth prevents vector's doubling from exceeding the job budget.
            const auto grown = capacity > maxBytes / 2 ? maxBytes : capacity * 2;
            bitmap_.data.reserve(std::max(required, grown));
        }
        return true;
    }

    bool PwgJobConverter::feed(const uint8_t *data, size_t len) try
    {
        if (failed())
            return false;
        if (!decoder_.feed(data, len))
        {
            if (error_.empty())
                error_ = decoder_.error();
            return false;
        }
        return !failed();
    }
    catch (const std::bad_alloc &)
    {
        outOfMemory_ = true;
        error_ = "out of memory";
        return false;
    }

    bool PwgJobConverter::finish() try
    {
        if (failed())
            return false;
        if (!decoder_.finish())
        {
            error_ = decoder_.error().empty() ? "truncated document" : decoder_.error();
            return false;
        }
        if (decoder_.pagesCompleted() == 0)
        {
            error_ = "document contains no pages";
            return false;
        }
        return true;
    }
    catch (const std::bad_alloc &)
    {
        outOfMemory_ = true;
        error_ = "out of memory";
        return false;
    }

} // namespace raster
