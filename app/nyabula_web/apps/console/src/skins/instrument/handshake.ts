/* True while the terminal login is printing its request log; the shell
 * keeps the login screen up until it finishes even though auth.user is
 * already set. */
import { ref } from 'vue';

export const handshaking = ref(false);
