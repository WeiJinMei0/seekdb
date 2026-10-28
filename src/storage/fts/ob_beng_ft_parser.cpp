/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (use_ascii_fast_path_) {
    ret = get_next_ascii_token(word, word_len, char_len, word_freq);
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(scratch_allocator_.alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

bool ObBEngFTParser::is_ascii_document(const char *fulltext, const int64_t fulltext_len)
{
  bool is_ascii = true;
  for (int64_t i = 0; is_ascii && i < fulltext_len; ++i) {
    is_ascii = 0 == (static_cast<unsigned char>(fulltext[i]) & 0x80);
  }
  return is_ascii;
}

bool ObBEngFTParser::is_ascii_delimiter(const char ch)
{
  const unsigned char c = static_cast<unsigned char>(ch);
  return c <= 0x20
      || c == 0x7F
      || (c >= 0x21 && c <= 0x2F)
      || (c >= 0x3A && c <= 0x40)
      || (c >= 0x5B && c <= 0x60)
      || (c >= 0x7B && c <= 0x7E);
}

int ObBEngFTParser::get_next_ascii_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  while (ascii_cursor_ < ascii_end_ && is_ascii_delimiter(*ascii_cursor_)) {
    ++ascii_cursor_;
  }
  if (ascii_cursor_ >= ascii_end_) {
    ret = OB_ITER_END;
  } else {
    const char *start = ascii_cursor_;
    bool needs_lowercase = false;
    while (ascii_cursor_ < ascii_end_ && !is_ascii_delimiter(*ascii_cursor_)) {
      const unsigned char ch = static_cast<unsigned char>(*ascii_cursor_);
      needs_lowercase = needs_lowercase || (ch >= 'A' && ch <= 'Z');
      ++ascii_cursor_;
    }
    const int64_t length = ascii_cursor_ - start;
    if (!needs_lowercase) {
      word = start;
    } else {
      char *buffer = static_cast<char *>(scratch_allocator_.alloc(length));
      if (OB_ISNULL(buffer)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < length; ++i) {
          const unsigned char ch = static_cast<unsigned char>(start[i]);
          buffer[i] = (ch >= 'A' && ch <= 'Z')
              ? static_cast<char>(ch + ('a' - 'A')) : start[i];
        }
        word = buffer;
      }
    }
    if (OB_SUCC(ret)) {
      word_len = length;
      char_len = length;
      word_freq = 1;
    }
  }
  return ret;
}

int ObBEngFTParser::init_analyzer()
{
  int ret = OB_SUCCESS;
  if (!analyzer_inited_) {
    if (OB_FAIL(english_analyzer_.init(analysis_ctx_, metadata_allocator_))) {
    } else {
      analyzer_inited_ = true;
    }
  }
  return ret;
}

int ObBEngFTParser::prepare_document(const char *fulltext, const int64_t fulltext_len)
{
  int ret = OB_SUCCESS;
  doc_.set_string(fulltext, fulltext_len);
  use_ascii_fast_path_ = CHARSET_UTF8MB4 == ObCharset::charset_type_by_coll(
      static_cast<ObCollationType>(analysis_ctx_.cs_->number))
      && is_ascii_document(fulltext, fulltext_len);
  if (use_ascii_fast_path_) {
    ascii_cursor_ = fulltext;
    ascii_end_ = fulltext + fulltext_len;
    token_stream_ = nullptr;
  } else if (OB_FAIL(init_analyzer())) {
  } else if (OB_FAIL(segment(doc_, token_stream_))) {
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObBEngFTParser::reuse_parser(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!param.is_valid() || UINT32_MAX < param.ft_length_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (analysis_ctx_.cs_ != param.cs_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    scratch_allocator_.reset_remain_one_page();
    ret = prepare_document(param.fulltext_, param.ft_length_);
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    if (OB_FAIL(prepare_document(param->fulltext_, param->ft_length_))) {
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  scratch_allocator_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  ascii_cursor_ = nullptr;
  ascii_end_ = nullptr;
  use_ascii_fast_path_ = false;
  analyzer_inited_ = false;
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  ObIAllocator *metadata_allocator = OB_NOT_NULL(param) && OB_NOT_NULL(param->metadata_allocator_)
                                      ? param->metadata_allocator_
                                      : (OB_NOT_NULL(param) ? param->allocator_ : nullptr);
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(metadata_allocator)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, metadata_allocator, *metadata_allocator))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(metadata_allocator)) {
    OB_DELETEx(ObBEngFTParser, metadata_allocator, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    ObIAllocator *metadata_allocator = OB_NOT_NULL(param->metadata_allocator_)
                                        ? param->metadata_allocator_ : param->allocator_;
    abort_unless(nullptr != metadata_allocator);
    iter->~ObITokenIterator();
    metadata_allocator->free(iter);
    iter = nullptr;
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
