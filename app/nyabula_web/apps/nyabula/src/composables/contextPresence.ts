/* How many <ContextSlot>s are currently mounted into the desktop context
 * panel. The shell collapses the column when nothing is contributed. */
import { reactive } from 'vue';

export const contextPresence = reactive({ count: 0 });
