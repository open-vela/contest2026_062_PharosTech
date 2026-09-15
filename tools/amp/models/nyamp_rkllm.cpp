/****************************************************************************
 * tools/amp/models/nyamp_rkllm.cpp
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

#include "nyamp_backends.h"
#include "rkllm.h"

namespace nyamp::models
{
namespace
{

struct RkllmRun
{
  const Emit &emit;
  const Stop &stop;
  bool stopped = false;
  bool failed = false;
  bool finished = false;
};

class RkllmBackend final : public Backend
{
public:
  ~RkllmBackend() override { Unload(); }
  Kind kind() const override { return Kind::kLlm; }
  Status Load(const std::string &directory) override;
  void Unload() override;
  Status Run(const Input &input, const Emit &emit, const Stop &stop) override;

private:
  static int Callback(RKLLMResult *result, void *userdata, LLMCallState state);
  LLMHandle handle_ = nullptr;
  std::string path_;
};

int RkllmBackend::Callback(RKLLMResult *result, void *userdata,
                           LLMCallState state)
{
  if (!userdata)
    return 0;
  auto &run = *static_cast<RkllmRun *>(userdata);
  try
    {
      if (state == RKLLM_RUN_ERROR)
        {
          run.failed = true;
        }
      else if (state == RKLLM_RUN_FINISH)
        {
          run.finished = true;
        }
      else if (state == RKLLM_RUN_NORMAL)
        {
          if (!result)
            {
              run.failed = true;
              return 1;
            }
          if (run.stop() ||
              !run.emit(TokenChunk{ result->token_id,
                                    result->text ? result->text : "" }))
            {
              run.stopped = true;
              return 1; // RKLLM 1.3 documents callback return 1 as pause.
            }
        }
    }
  catch (...)
    {
      run.failed = true;
      return 1;
    }
  return 0;
}

Status RkllmBackend::Load(const std::string &directory)
{
  if (handle_)
    {
      return Status::kBusy;
    }
  path_ = directory + "/model.rkllm";
  RKLLMParam parameters = rkllm_createDefaultParam();
  parameters.model_path = path_.c_str();
  parameters.max_context_len = 2048;
  parameters.max_new_tokens = 128;
  parameters.top_k = 1;
  parameters.top_p = 0.95f;
  parameters.temperature = 0.7f;
  parameters.repeat_penalty = 1.05f;
  parameters.skip_special_token = false;
  parameters.extend_param.base_domain_id = 0;
  parameters.extend_param.embed_flash = 0;
  parameters.extend_param.enabled_cpus_num = 4;
  parameters.extend_param.enabled_cpus_mask = 0xf;
  RKLLMCallback callbacks{};
  callbacks.result_callback = Callback;
  if (rkllm_init(&handle_, &parameters, &callbacks) != 0)
    {
      Unload();
      return Status::kBackendError;
    }
  if (rkllm_set_chat_template(handle_, "", "", "") != 0)
    {
      Unload();
      return Status::kBackendError;
    }
  return Status::kOk;
}

void RkllmBackend::Unload()
{
  if (handle_)
    {
      rkllm_destroy(handle_);
      handle_ = nullptr;
    }
}

Status RkllmBackend::Run(const Input &input, const Emit &emit,
                         const Stop &stop)
{
  const auto *tokens = std::get_if<LlmInput>(&input);
  if (!tokens || !handle_)
    {
      return Status::kNotReady;
    }
  if (stop())
    {
      return Status::kCancelled;
    }
  if (rkllm_clear_kv_cache(handle_, 0, nullptr, nullptr) != 0)
    {
      return Status::kBackendError;
    }
  // RKLLM's API is non-const; do not expose the caller's immutable storage.
  auto ids = tokens->token_ids;
  RKLLMInput request{};
  request.role = "user";
  request.enable_thinking = false;
  request.input_type = RKLLM_INPUT_TOKEN;
  request.token_input.input_ids = ids.data();
  request.token_input.n_tokens = ids.size();
  RKLLMInferParam inference{};
  inference.mode = RKLLM_INFER_GENERATE;
  inference.keep_history = 0;
  inference.max_new_tokens = tokens->max_new_tokens;
  RkllmRun run{ emit, stop };
  const int result = rkllm_run(handle_, &request, &inference, &run);
  if (run.stopped || run.failed)
    {
      rkllm_abort(handle_);
    }
  if (run.stopped)
    {
      return Status::kCancelled;
    }
  return result == 0 && !run.failed && run.finished ? Status::kOk
                                                    : Status::kBackendError;
}

} // namespace

std::unique_ptr<Backend> CreateRkllmBackend()
{
  return std::make_unique<RkllmBackend>();
}

} // namespace nyamp::models
