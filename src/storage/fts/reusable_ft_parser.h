/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include "data_plane/fts/ob_fts_parser.h"

namespace oceanbase
{
namespace storage
{

// Internal extension used only by built-in parsers.  Keeping it separate from
// ObITokenIterator preserves the public parser interface.
class ReusableFTParser : public ObITokenIterator
{
public:
  ReusableFTParser() = default;
  virtual ~ReusableFTParser() = default;
  virtual int reuse_parser(const ObFTParserParam &param) = 0;
};

} // namespace storage
} // namespace oceanbase
