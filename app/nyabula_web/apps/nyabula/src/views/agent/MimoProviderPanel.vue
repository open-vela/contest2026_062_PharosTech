<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

const session = useSessionStore();
const bot = useNyabotStore();
const form = reactive({ host: 'api.xiaomimimo.com', visionModel: 'mimo-v2.6-flash', ttsBackend: 'local', asrBackend: 'local',
  ttsModel: 'mimo-v2.5-tts', voice: 'mimo_default', style: '', key: '', clearKey: false });
const saved = ref('');
const busy = ref(false);
const error = ref('');
const keySet = ref(false);
const ready = ref(false);
const development = ref(false);
const dirty = computed(() => !!saved.value && JSON.stringify(form) !== saved.value);
const voices = ['mimo_default', '冰糖', '茉莉', '苏打', '白桦', 'Mia', 'Chloe', 'Milo', 'Dean'];
const visionModels = ['mimo-v2.6-flash', 'mimo-v2.6-pro', 'mimo-v2.6-pro-ultraspeed'];

async function sync(save = false): Promise<void> {
  if (!session.connected || !session.isOwner || !bot.status?.ready || busy.value) return;
  const client = session.client;
  busy.value = true; error.value = '';
  try {
    const data = await session.request(`agent.config.mimo.${save ? 'set' : 'get'}`, save ? { ...form } : {});
    if (client !== session.client) return;
    for (const key of ['host', 'visionModel', 'ttsBackend', 'ttsModel', 'voice', 'style'] as const)
      form[key] = key === 'visionModel' && data[key] === 'mimo-v2.5'
        ? 'mimo-v2.6-flash' : String(data[key]);
    form.asrBackend = data.asrBackend === 'mimo' ? 'mimo' : 'local';
    form.key = ''; form.clearKey = false;
    keySet.value = Boolean(data.keySet); ready.value = Boolean(data.credentialsReady);
    development.value = data.development === true;
    saved.value = JSON.stringify(form);
  } catch (cause) { error.value = cause instanceof Error ? cause.message : String(cause); }
  finally { busy.value = false; }
}
function discard(): void { if (saved.value) Object.assign(form, JSON.parse(saved.value)); }
watch([() => session.client, () => bot.status?.ready], () => {
  saved.value = ''; form.key = ''; void sync();
}, { immediate: true });
</script>

<template>
  <section class="mimo">
    <h3>MiMo 图片与语音</h3>
    <p>图片理解、语音识别与语音合成可独立于主对话模型设置。</p>
    <p v-if="development">模拟器保存这些设备设置；图片与语音硬件链在实机测试。模拟器聊天和插件生成可直接使用在线模型。</p>
    <form @submit.prevent="sync(true)">
      <label>MiMo 服务端点<select v-model="form.host" :disabled="busy || !saved">
        <option value="api.xiaomimimo.com">按量付费 API</option>
        <option value="token-plan-cn.xiaomimimo.com">Token Plan · 中国</option>
        <option value="token-plan-sgp.xiaomimimo.com">Token Plan · 新加坡</option>
        <option value="token-plan-ams.xiaomimimo.com">Token Plan · 欧洲</option>
      </select></label>
      <p>密钥必须与所选端点匹配；套餐与按量付费密钥不能混用。留空仅复用同一端点的主模型密钥，语音模型可用性以套餐为准。</p>
      <MdTextField v-model="form.key" type="password" autocomplete="off" label="MiMo 专用密钥"
        :placeholder="keySet ? '留空保留已保存密钥' : '留空时，仅复用同一端点的主模型密钥'" :disabled="busy || !saved" />
      <label v-if="keySet" class="check"><input v-model="form.clearKey" type="checkbox" :disabled="busy" />移除专用密钥，改为复用</label>
      <label>图片理解模型<select v-model="form.visionModel" :disabled="busy || !saved">
        <option v-for="model in visionModels" :key="model">{{ model }}</option>
      </select></label>
      <p>在对话中提供公开 HTTPS 图片链接并提出问题，助手可调用 mimo_vision。默认使用 MiMo-V2.6-Flash，也可选择 2.6 Pro。</p>
      <label>语音识别来源<select v-model="form.asrBackend" :disabled="busy || !saved">
        <option value="local">本地 ASR（离线）</option><option value="mimo">MiMo-V2.5-ASR（云端）</option>
      </select></label>
      <p v-if="form.asrBackend === 'mimo'">唤醒仍在本地。唤醒后或点击“开始聆听”后录音，停顿时上传至所选 MiMo 端点识别；每段最长30秒。取消后丢弃迟到结果。</p>
      <label>语音合成来源<select v-model="form.ttsBackend" :disabled="busy || !saved">
        <option value="local">本地 TTS（离线）</option><option value="mimo">MiMo 云端 TTS</option>
      </select></label>
      <template v-if="form.ttsBackend === 'mimo'">
        <label>合成模型<select v-model="form.ttsModel" :disabled="busy || !saved">
          <option value="mimo-v2.5-tts">MiMo-V2.5-TTS · 预设音色</option>
          <option value="mimo-v2.5-tts-voicedesign">MiMo-V2.5-TTS · 音色设计</option>
        </select></label>
        <label v-if="form.ttsModel === 'mimo-v2.5-tts'">音色<select v-model="form.voice" :disabled="busy || !saved">
          <option v-for="voice in voices" :key="voice">{{ voice }}</option>
        </select></label>
        <label>{{ form.ttsModel.endsWith('voicedesign') ? '音色描述（必填）' : '语气与风格（可选）' }}
          <textarea v-model="form.style" rows="3" maxlength="250" :required="form.ttsModel.endsWith('voicedesign')"
            :disabled="busy || !saved" placeholder="例如：温柔、自然，语速舒缓" />
        </label>
        <p>输出为 24 kHz PCM，设备自动重采样。云端使用风格描述控制语气；本地音色编号和语速不用于云端。音色设计会等待整段合成后播放。</p>
      </template>
      <div class="actions">
        <MdButton :disabled="busy || !dirty || !session.isOwner">保存图片与语音设置</MdButton>
        <MdButton type="button" variant="text" :disabled="busy || !dirty" @click="discard">撤销</MdButton>
        <MdButton type="button" variant="text" :disabled="busy || dirty" @click="sync()">刷新</MdButton>
      </div>
      <p v-if="error" role="alert">{{ error }}</p>
      <p v-else-if="saved" role="status">{{ dirty ? '未保存' : '已读取设备配置' }} · {{ ready ? 'MiMo 密钥可用' : '尚无可用 MiMo 密钥' }}</p>
    </form>
  </section>
</template>

<style scoped>
.mimo { margin-top: 24px; padding-top: 16px; border-top: 1px solid var(--md-outline-variant); }
form, label { display: flex; flex-direction: column; gap: 8px; }
form { gap: 16px; }
.check, .actions { display: flex; flex-direction: row; flex-wrap: wrap; gap: 8px; }

[role=alert] { color: var(--md-error); }
</style>
