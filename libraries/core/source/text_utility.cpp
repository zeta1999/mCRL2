// Author(s): Wieger Wesselink
// Copyright: see the accompanying file COPYING or copy at
// https://svn.win.tue.nl/trac/MCRL2/browser/trunk/COPYING
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
/// \file mcrl2/core/text_utility.h
/// \brief String manipulation functions.

#include <fstream>
#include <iostream>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/xpressive/xpressive.hpp>
#include "mcrl2/core/text_utility.h"
#include "mcrl2/exception.h"

namespace mcrl2
{

namespace core
{

/// \brief Split a string into paragraphs.
/// \param text A string
/// \return The paragraphs of <tt>text</tt>
std::vector<std::string> split_paragraphs(const std::string& text)
{
  std::vector<std::string> result;

  // find multiple line endings
  boost::xpressive::sregex paragraph_split = boost::xpressive::sregex::compile("\\n\\s*\\n");

  // the -1 below directs the token iterator to display the parts of
  // the string that did NOT match the regular expression.
  boost::xpressive::sregex_token_iterator cur(text.begin(), text.end(), paragraph_split, -1);
  boost::xpressive::sregex_token_iterator end;

  for (; cur != end; ++cur)
  {
    std::string paragraph = *cur;
    boost::trim(paragraph);
    if (paragraph.size() > 0)
    {
      result.push_back(paragraph);
    }
  }
  return result;
}

/// \brief Split the text.
/// \param line A string
/// \param separators A string
/// \return The splitted text
std::vector<std::string> split(const std::string& line, const std::string& separators)
{
  std::vector<std::string> result;
  boost::algorithm::split(result, line, boost::algorithm::is_any_of(separators));
  return result;
}

/// \brief Read text from a file.
/// \param filename A string
/// \param warn If true, a warning is printed to standard error if the file is not found
/// \return The contents of the file
std::string read_text(const std::string& filename, bool warn)
{
  std::ifstream in(filename.c_str());
  if (!in)
  {
    if (warn)
    {
      std::cerr << "Could not open input file: " << filename << std::endl;
      return "";
    }
    else
    {
      throw mcrl2::runtime_error("Could not open input file: " + filename);
    }
  }
  in.unsetf(std::ios::skipws); //  Turn of white space skipping on the stream

  std::string s;
  std::copy(
    std::istream_iterator<char>(in),
    std::istream_iterator<char>(),
    std::back_inserter(s)
  );

  return s;
}

/// \brief Remove comments from a text (everything from '%' until end of line).
/// \param text A string
/// \return The removal result
std::string remove_comments(const std::string& text)
{
  // matches everything from '%' until end of line
  boost::xpressive::sregex src = boost::xpressive::sregex::compile("%[^\\n]*\\n");

  std::string dest("\n");
  return boost::xpressive::regex_replace(text, src, dest);
}

/// \brief Removes whitespace from a string.
/// \param text A string
/// \return The removal result
std::string remove_whitespace(const std::string& text)
{
  boost::xpressive::sregex src = boost::xpressive::sregex::compile("\\s");
  std::string dest("");
  return boost::xpressive::regex_replace(text, src, dest);
}

/// \brief Regular expression replacement in a string.
/// \param src A string
/// \param dest A string
/// \param text A string
/// \return The transformed string
std::string regex_replace(const std::string& src, const std::string& dest, const std::string& text)
{
  return boost::xpressive::regex_replace(text, boost::xpressive::sregex::compile(src), dest);
}

/// \brief Split a string using a regular expression separator.
/// \param text A string
/// \param sep A string
/// \return The splitted string
std::vector<std::string> regex_split(const std::string& text, const std::string& sep)
{
  std::vector<std::string> result;
  // find multiple line endings
  boost::xpressive::sregex paragraph_split = boost::xpressive::sregex::compile(sep);
  // the -1 below directs the token iterator to display the parts of
  // the string that did NOT match the regular expression.
  boost::xpressive::sregex_token_iterator cur(text.begin(), text.end(), paragraph_split, -1);
  boost::xpressive::sregex_token_iterator end;
  for (; cur != end; ++cur)
  {
    std::string word = *cur;
    boost::trim(word);
    if (word.size() > 0)
    {
      result.push_back(word);
    }
  }
  return result;
}

/// \brief Apply word wrapping to a text that doesn't contain newlines.
/// \param text A string of text.
/// \param max_line_length The maximum line length.
/// \return The wrapped text.
std::vector<std::string> word_wrap_line(const std::string& line, unsigned int max_line_length)
{
  std::vector<std::string> result;
  std::string text = line;

  for (;;)
  {
    if (text.size() <= max_line_length)
    {
      result.push_back(boost::trim_right_copy(text));
      break;
    }
    std::string::size_type i = text.find_last_of(" \t", max_line_length);
    if (i == std::string::npos)
    {
      result.push_back(text.substr(0, max_line_length));
      text = text.substr(max_line_length);
    }
    else
    {
      result.push_back(text.substr(0, i));
      text = text.substr(i + 1);
    }
  }
  return result;
}

/// \brief Apply word wrapping to a text.
/// \param text A string of text.
/// \param max_line_length The maximum line length.
/// \return The wrapped text.
std::string word_wrap_text(const std::string& text, unsigned int max_line_length)
{
  std::vector<std::string> result;

  // split the lines and remove trailing white space
  std::vector<std::string> lines = split(text, "\n");
  for (std::vector<std::string>::iterator i = lines.begin(); i != lines.end(); ++i)
  {
    boost::trim_right(*i);
  }

  // word wrap each of the lines
  for (std::vector<std::string>::iterator i = lines.begin(); i != lines.end(); ++i)
  {
    std::vector<std::string> v = word_wrap_line(*i, max_line_length);
    result.insert(result.end(), v.begin(), v.end());
  }

  return string_join(result, "\n");
}

} // namespace core

} // namespace mcrl2
