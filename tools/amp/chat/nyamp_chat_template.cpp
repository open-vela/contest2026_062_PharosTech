/****************************************************************************
 * tools/amp/chat/nyamp_chat_template.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

// Transcription of model/chat_template.jinja (sha256 7451a05c...f47c).
//
// Quirks of the original that are reproduced on purpose - each one was
// confirmed against transformers 5.8.0 before being written down here:
//
//  * `{% set processed_content = ... %}` inside the `<tool_sep>` loops never
//    escapes the loop scope (Jinja scoping), so the whole tool_sep machinery
//    collapses to "keep the content before the first <tool_sep>".
//  * `has_tool_sep` is never defined, so tool calls are always appended
//    after the content, separated by "\n".
//  * Non-string argument values are printed with Python str(): True, None,
//    1.5, {'k': [1, 'v']}.
//  * `<think>` reasoning is only re-rendered for assistant turns that come
//    after the last real user query; earlier reasoning is dropped.
//  * A message whose role is not system/user/assistant/tool renders nothing.

#include "nyamp_chat_template.h"

#include <utility>

namespace nyamp
{

namespace
{

constexpr const char *kToolsHead =
    "# Tools\n\nYou are provided with function signatures within "
    "<tools></tools> XML tags:\n<tools>";

constexpr const char *kToolsTail =
    "\n</tools>\n\nTool usage guidelines:\n- You may call zero or more "
    "functions. If no function calls are needed, just answer normally and do "
    "not include any <function ... </function>.\n- When calling a function, "
    "return an XML object within <function ... </function> using:\n<function "
    "name=\"function-name\"><param "
    "name=\"param-name\">param-value</param></function>\n- param-value may be "
    "multi-line. If it contains <, & or newline characters, wrap it in a "
    "CDATA "
    "block: <param name=\"param-name\"><![CDATA[...multi-line "
    "value...]]></param>";

constexpr std::string_view kToolDefSep = "<tool_def_sep>";
constexpr std::string_view kThinkOpen = "<think>";
constexpr std::string_view kThinkClose = "</think>";
constexpr std::string_view kToolSep = "<tool_sep>";
constexpr std::string_view kResponseOpen = "<tool_response>";
constexpr std::string_view kResponseClose = "</tool_response>";

// Output buffer that remembers which bytes came from request data.
class Builder
{
public:
  void lit(std::string_view s) { text_.append(s); }

  void data(std::string_view s)
  {
    if (s.empty())
      return;
    size_t begin = text_.size();
    text_.append(s);
    if (!ranges_.empty() && ranges_.back().end == begin)
      {
        ranges_.back().end = text_.size();
      }
    else
      {
        ranges_.push_back(ByteRange{ begin, text_.size() });
      }
  }

  void append(const Builder &other)
  {
    size_t base = text_.size();
    text_.append(other.text_);
    for (const ByteRange &r : other.ranges_)
      {
        ranges_.push_back(ByteRange{ r.begin + base, r.end + base });
      }
  }

  std::string &text() { return text_; }
  std::vector<ByteRange> &ranges() { return ranges_; }

private:
  std::string text_;
  std::vector<ByteRange> ranges_;
};

std::string_view lstrip_newlines(std::string_view s)
{
  while (!s.empty() && s.front() == '\n')
    s.remove_prefix(1);
  return s;
}

std::string_view rstrip_newlines(std::string_view s)
{
  while (!s.empty() && s.back() == '\n')
    s.remove_suffix(1);
  return s;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
  return s.size() >= suffix.size() &&
         s.substr(s.size() - suffix.size()) == suffix;
}

std::string role_of(const Json &message)
{
  const Json *role = message.find("role");
  return role != nullptr && role->is_string() ? role->str() : std::string();
}

// `message.content is string` - nullptr stands for "not a string".
const std::string *string_content(const Json &message)
{
  const Json *content = message.find("content");
  return content != nullptr && content->is_string() ? &content->str()
                                                    : nullptr;
}

void flatten_parts(Json *message)
{
  const Json *content = message->find("content");
  if (content == nullptr || !content->is_array())
    return;
  std::string joined;
  bool any = false;
  for (const Json &part : content->items())
    {
      const Json *type = part.find("type");
      const Json *text = part.find("text");
      if (type == nullptr || !type->is_string() || type->str() != "text" ||
          text == nullptr || !text->is_string())
        {
          continue; // images etc.: this model is text only
        }
      if (any)
        joined += "\n";
      joined += text->str();
      any = true;
    }
  message->set("content", Json::string(std::move(joined)));
}

void render_tool_call(const Json &call_in, const ChatTemplateOptions &options,
                      Builder *out)
{
  const Json *call = &call_in;
  const Json *function = call->find("function");
  if (function != nullptr && function->truthy())
    call = function;

  out->lit("<function name=\"");
  const Json *name = call->find("name");
  if (name != nullptr)
    out->data(python_str(*name));
  out->lit("\">");

  const Json *arguments = call->find("arguments");
  Json parsed;
  if (arguments != nullptr && arguments->is_string())
    {
      // OpenAI carries arguments as a JSON *string*; the reference template
      // needs a dict and raises on a string.  Parsing here is what lets the
      // daemon replay its own responses as history.
      if (json_parse(arguments->str(), &parsed, nullptr) && parsed.is_object())
        {
          arguments = &parsed;
        }
      else
        {
          arguments = nullptr;
        }
    }
  if (arguments != nullptr && arguments->is_object())
    {
      for (const auto &member : arguments->members())
        {
          out->lit("<param name=\"");
          out->data(member.first);
          out->lit("\">");
          const Json &value = member.second;
          if (value.is_string())
            {
              const std::string &s = value.str();
              if (s.find_first_of("<&\n") != std::string::npos)
                {
                  out->lit("<![CDATA[");
                  out->data(s);
                  out->lit("]]>");
                }
              else
                {
                  out->data(s);
                }
            }
          else if (options.json_for_nested_arguments &&
                   (value.is_array() || value.is_object()))
            {
              out->data(json_dump_python(value));
            }
          else
            {
              out->data(python_str(value));
            }
          out->lit("</param>");
        }
    }
  out->lit("</function>");
}

} // namespace

Json normalize_tools(const Json &tools)
{
  Json out = Json::array();
  if (!tools.is_array())
    return out;
  for (const Json &tool : tools.items())
    {
      if (!tool.is_object() || tool.find("function") != nullptr ||
          tool.find("name") == nullptr)
        {
          out.push(tool);
          continue;
        }
      // Anthropic-like {name, description, input_schema}.  The wrapper and its
      // key order are the ones OpenAI documents, i.e. what the model's tool
      // training data overwhelmingly looks like.
      Json function = Json::object();
      function.set("name", *tool.find("name"));
      if (const Json *description = tool.find("description"))
        {
          function.set("description", *description);
        }
      const Json *schema = tool.find("input_schema");
      if (schema == nullptr)
        schema = tool.find("parameters");
      if (schema != nullptr)
        function.set("parameters", *schema);
      Json wrapped = Json::object();
      wrapped.set("type", Json::string("function"));
      wrapped.set("function", std::move(function));
      out.push(std::move(wrapped));
    }
  return out;
}

bool render_chat_prompt(const Json &request,
                        const ChatTemplateOptions &options,
                        RenderedPrompt *out, std::string *error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  const Json *messages_in = request.find("messages");
  if (messages_in == nullptr || !messages_in->is_array() ||
      messages_in->items().empty())
    {
      // The reference raises IndexError on messages[0]; there is nothing
      // sensible to generate from an empty conversation anyway.
      *error = "request has no messages";
      return false;
    }
  std::vector<Json> messages = messages_in->items();
  if (options.flatten_text_parts)
    {
      for (Json &message : messages)
        {
          if (message.is_object())
            flatten_parts(&message);
        }
    }

  Builder b;
  b.lit("<s>");

  const bool first_is_system = role_of(messages[0]) == "system";
  // Lenient where the reference raises TypeError: a system message without
  // string content is treated as empty.
  const std::string *first_content = string_content(messages[0]);
  const std::string empty;
  const std::string &system_text = first_content ? *first_content : empty;

  const Json *tools_in = request.find("tools");
  Json tools =
      tools_in != nullptr ? normalize_tools(*tools_in) : Json::array();
  if (tools.truthy())
    {
      Builder defs;
      defs.lit(kToolsHead);
      for (const Json &tool : tools.items())
        {
          defs.lit("\n");
          // Tool descriptions can originate from external MCP catalogs, hence
          // untrusted even though the agent assembled the list.
          defs.data(json_dump_python(tool));
        }
      defs.lit(kToolsTail);

      b.lit("<|im_start|>system\n");
      if (first_is_system)
        {
          if (system_text.find(kToolDefSep) != std::string::npos)
            {
              // str.replace(): every occurrence is substituted.
              size_t pos = 0;
              for (;;)
                {
                  size_t hit = system_text.find(kToolDefSep, pos);
                  if (hit == std::string::npos)
                    break;
                  b.lit(std::string_view(system_text).substr(pos, hit - pos));
                  b.append(defs);
                  pos = hit + kToolDefSep.size();
                }
              b.lit(std::string_view(system_text).substr(pos));
            }
          else
            {
              b.lit(system_text);
              b.lit("\n\n");
              b.append(defs);
            }
        }
      else
        {
          b.append(defs);
        }
      b.lit("<|im_end|>\n");
    }
  else if (first_is_system)
    {
      b.lit("<|im_start|>system\n");
      b.lit(system_text);
      b.lit("<|im_end|>\n");
    }

  // Index of the last genuine user query: a user message that is not just a
  // wrapped tool response.
  size_t last_query_index = messages.size() - 1;
  for (size_t k = messages.size(); k-- > 0;)
    {
      const std::string *content = string_content(messages[k]);
      if (role_of(messages[k]) == "user" && content != nullptr &&
          !(starts_with(*content, kResponseOpen) &&
            ends_with(*content, kResponseClose)))
        {
          last_query_index = k;
          break;
        }
    }

  for (size_t index = 0; index < messages.size(); ++index)
    {
      const Json &message = messages[index];
      const std::string role = role_of(message);
      const std::string *content_ptr = string_content(message);
      std::string_view content =
          content_ptr ? std::string_view(*content_ptr) : std::string_view();

      if (role == "user" || (role == "system" && index != 0))
        {
          b.lit("<|im_start|>");
          b.lit(role);
          b.lit("\n");
          if (role == "user")
            {
              b.data(content);
            }
          else
            {
              b.lit(content);
            }
          b.lit("<|im_end|>\n");
        }
      else if (role == "assistant")
        {
          std::string_view reasoning;
          const Json *explicit_reasoning = message.find("reasoning_content");
          if (explicit_reasoning != nullptr && explicit_reasoning->is_string())
            {
              reasoning = explicit_reasoning->str();
            }
          else
            {
              size_t first_close = content.find(kThinkClose);
              if (first_close != std::string_view::npos)
                {
                  std::string_view head =
                      rstrip_newlines(content.substr(0, first_close));
                  size_t last_open = head.rfind(kThinkOpen);
                  if (last_open != std::string_view::npos)
                    {
                      head = head.substr(last_open + kThinkOpen.size());
                    }
                  reasoning = lstrip_newlines(head);
                  size_t last_close = content.rfind(kThinkClose);
                  content = lstrip_newlines(
                      content.substr(last_close + kThinkClose.size()));
                }
            }

          const Json *tool_calls = message.find("tool_calls");
          bool has_calls = tool_calls != nullptr && tool_calls->is_array() &&
                           !tool_calls->items().empty();
          if (has_calls)
            {
              size_t sep = content.find(kToolSep);
              if (sep != std::string_view::npos)
                content = content.substr(0, sep);
            }

          b.lit("<|im_start|>assistant\n");
          if (index > last_query_index && !reasoning.empty())
            {
              b.lit("<think>\n");
              b.lit(rstrip_newlines(lstrip_newlines(reasoning)));
              b.lit("\n</think>\n\n");
              content = lstrip_newlines(content);
            }
          b.lit(content);

          if (has_calls)
            {
              bool first = true;
              for (const Json &call : tool_calls->items())
                {
                  if (!first || !content.empty())
                    b.lit("\n");
                  first = false;
                  render_tool_call(call, options, &b);
                }
            }
          b.lit("<|im_end|>\n");
        }
      else if (role == "tool")
        {
          if (index == 0 || role_of(messages[index - 1]) != "tool")
            {
              b.lit("<|im_start|>user");
            }
          b.lit("\n<tool_response>\n");
          if (content_ptr != nullptr)
            {
              b.data(content);
            }
          else
            {
              const Json *raw = message.find("content");
              b.data(json_dump_python(raw != nullptr ? *raw : Json::null()));
            }
          b.lit("\n</tool_response>");
          if (index + 1 == messages.size() ||
              role_of(messages[index + 1]) != "tool")
            {
              b.lit("<|im_end|>\n");
            }
        }
    }

  if (options.add_generation_prompt)
    {
      b.lit("<|im_start|>assistant\n");
      if (options.thinking == ChatTemplateOptions::kOff)
        {
          b.lit("<think>\n\n</think>\n\n");
        }
      else if (options.thinking == ChatTemplateOptions::kOn)
        {
          b.lit("<think>\n");
        }
    }

  out->text = std::move(b.text());
  out->untrusted = std::move(b.ranges());
  return true;
}

bool encode_chat_request(const Tokenizer &tokenizer, std::string_view request,
                         const ChatTemplateOptions &options,
                         bool guard_untrusted, EncodedPrompt *out,
                         std::string *error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  // Sanitize before parsing: afterwards every string in the DOM is valid
  // UTF-8, so the guard ranges computed by the renderer stay aligned with
  // the text the tokenizer sees.
  std::string clean;
  if (!utf8_is_valid(request))
    {
      utf8_sanitize(request, &clean);
      request = clean;
    }
  Json root;
  if (!json_parse(request, &root, error))
    return false;
  if (!root.is_object())
    {
      *error = "request is not a JSON object";
      return false;
    }
  ChatTemplateOptions effective = options;
  // Same per-request switch vLLM/SGLang expose, so a client can opt into
  // thinking without the daemon growing a private field.
  if (const Json *kwargs = root.find("chat_template_kwargs"))
    {
      const Json *thinking = kwargs->find("enable_thinking");
      if (thinking != nullptr && thinking->is_bool())
        {
          effective.thinking = thinking->as_bool() ? ChatTemplateOptions::kOn
                                                   : ChatTemplateOptions::kOff;
        }
    }
  if (!render_chat_prompt(root, effective, &out->prompt, error))
    return false;
  out->ids.clear();
  if (guard_untrusted)
    {
      tokenizer.encode_guarded(out->prompt.text, out->prompt.untrusted,
                               &out->ids);
    }
  else
    {
      out->ids = tokenizer.encode(out->prompt.text, true);
    }
  return true;
}

} // namespace nyamp
