// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// render_detail.hpp — formatting helpers shared by the Renderer and the
// output formats.
//
// PRIVATE engine header (not installed). renderer.cpp defines these; format.cpp
// uses them, so a vector-search score is written the same way in every format.

#ifndef DDBSH_SRC_RENDER_RENDER_DETAIL_HPP
#define DDBSH_SRC_RENDER_RENDER_DETAIL_HPP

#include <string>

namespace ddbsh::render::detail {

/// A SearchVectors similarity score as the Renderer prints it after `score `:
/// std::to_string with the trailing zeros trimmed, sign kept (D-4b-6).
std::string format_score(double score);

}  // namespace ddbsh::render::detail

#endif  // DDBSH_SRC_RENDER_RENDER_DETAIL_HPP
