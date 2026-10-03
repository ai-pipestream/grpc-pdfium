#include "page_space.h"

namespace grpc_pdfium {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

CropOrigin OriginOf(const CropOrigins& origins, uint32_t page_index) {
  return page_index < origins.size() ? origins[page_index] : CropOrigin{};
}

void Shift(const CropOrigin& o, pdfv1::BoundingBox* box) {
  box->set_x0(box->x0() - o.x);
  box->set_y0(box->y0() - o.y);
  box->set_x1(box->x1() - o.x);
  box->set_y1(box->y1() - o.y);
}

void Shift(const CropOrigin& o, pdfv1::Quad* quad) {
  quad->set_x0(quad->x0() - o.x);
  quad->set_y0(quad->y0() - o.y);
  quad->set_x1(quad->x1() - o.x);
  quad->set_y1(quad->y1() - o.y);
  quad->set_x2(quad->x2() - o.x);
  quad->set_y2(quad->y2() - o.y);
  quad->set_x3(quad->x3() - o.x);
  quad->set_y3(quad->y3() - o.y);
}

void Shift(const CropOrigin& o, pdfv1::PathPoint* point) {
  point->set_x(point->x() - o.x);
  point->set_y(point->y() - o.y);
}

// A destination is in the space of the page it targets; an unset
// coordinate means "keep the current one" and stays unset.
void Shift(const CropOrigins& origins, pdfv1::PageDestination* dest) {
  const CropOrigin o = OriginOf(origins, dest->page_index());
  if (dest->has_x()) dest->set_x(dest->x() - o.x);
  if (dest->has_y()) dest->set_y(dest->y() - o.y);
}

void Shift(const CropOrigins& origins, pdfv1::OutlineNode* node) {
  if (node->has_destination()) Shift(origins, node->mutable_destination());
  for (auto& child : *node->mutable_children()) Shift(origins, &child);
}

}  // namespace

CropOrigins CropOriginsOf(
    const google::protobuf::RepeatedPtrField<pdfv1::PageInfo>& pages) {
  CropOrigins origins;
  for (const auto& page : pages) {
    if (page.page_index() >= origins.size()) {
      origins.resize(page.page_index() + 1);
    }
    if (page.has_crop_box()) {
      origins[page.page_index()] = {page.crop_box().x0(), page.crop_box().y0()};
    }
  }
  return origins;
}

void ShiftToCropSpace(const CropOrigins& origins, pdfv1::PageChunk* chunk) {
  const CropOrigin o = OriginOf(origins, chunk->page_index());
  for (auto& cell : *chunk->mutable_text_cells()) {
    if (cell.has_bbox()) Shift(o, cell.mutable_bbox());
    if (cell.has_quad()) Shift(o, cell.mutable_quad());
  }
  for (auto& image : *chunk->mutable_images()) {
    if (image.has_bbox()) Shift(o, image.mutable_bbox());
    if (image.has_quad()) Shift(o, image.mutable_quad());
  }
  for (auto& link : *chunk->mutable_hyperlinks()) {
    if (link.has_bbox()) Shift(o, link.mutable_bbox());
    if (link.has_destination()) Shift(origins, link.mutable_destination());
  }
  for (auto& annot : *chunk->mutable_annotations()) {
    if (annot.has_rect()) Shift(o, annot.mutable_rect());
    for (auto& quad : *annot.mutable_quads()) Shift(o, &quad);
  }
  for (auto& field : *chunk->mutable_form_fields()) {
    if (field.has_rect()) Shift(o, field.mutable_rect());
  }
  for (auto& shape : *chunk->mutable_shapes()) {
    if (shape.has_bbox()) Shift(o, shape.mutable_bbox());
    for (auto& segment : *shape.mutable_segments()) {
      switch (segment.op_case()) {
        case pdfv1::PathSegment::kMoveTo:
          Shift(o, segment.mutable_move_to());
          break;
        case pdfv1::PathSegment::kLineTo:
          Shift(o, segment.mutable_line_to());
          break;
        case pdfv1::PathSegment::kCubicTo: {
          auto* cubic = segment.mutable_cubic_to();
          if (cubic->has_control1()) Shift(o, cubic->mutable_control1());
          if (cubic->has_control2()) Shift(o, cubic->mutable_control2());
          if (cubic->has_end()) Shift(o, cubic->mutable_end());
          break;
        }
        case pdfv1::PathSegment::kClose:
        case pdfv1::PathSegment::OP_NOT_SET:
          break;
      }
    }
  }
}

void ShiftToCropSpace(const CropOrigins& origins,
                      pdfv1::OutlineChunk* outline) {
  for (auto& root : *outline->mutable_roots()) Shift(origins, &root);
}

}  // namespace grpc_pdfium
