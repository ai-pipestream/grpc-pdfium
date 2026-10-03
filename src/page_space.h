#pragma once

#include <vector>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"

namespace grpc_pdfium {

// The contract frame for page geometry (PageInfo.page_space =
// PAGE_SPACE_CROP_BOX): PDF user space before /Rotate, shifted so the
// CropBox's bottom-left corner is (0, 0). PDFium reports every page object,
// text box, link and annotation in unshifted user space, so the engine
// extracts in that frame and moves the finished messages here, one place
// for every family.

// A page's CropBox origin in user space: crop_box.(x0, y0).
struct CropOrigin {
  double x = 0;
  double y = 0;
};

// CropBox origins by page index.
using CropOrigins = std::vector<CropOrigin>;

// The origins of the pages an inventory lists. A page whose crop_box is
// unset keeps (0, 0).
CropOrigins CropOriginsOf(
    const google::protobuf::RepeatedPtrField<
        ai::protomolt::parse::pdf::v1::PageInfo>& pages);

// Moves every geometry field of the chunk from user space into the frame
// of its page (chunk->page_index()): text cells, images, hyperlinks,
// annotations, form widgets and shapes. A hyperlink destination moves by
// the origin of the page it targets.
void ShiftToCropSpace(const CropOrigins& origins,
                      ai::protomolt::parse::pdf::v1::PageChunk* chunk);

// Moves every outline destination into the frame of the page it targets.
void ShiftToCropSpace(const CropOrigins& origins,
                      ai::protomolt::parse::pdf::v1::OutlineChunk* outline);

}  // namespace grpc_pdfium
