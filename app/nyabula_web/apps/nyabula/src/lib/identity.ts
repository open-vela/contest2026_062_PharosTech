export interface IdentityMember {id:string;name:string;recent:boolean;seenAgoMs:number;confidence:number}
export interface IdentityState {revision:number;phase:'idle'|'collecting'|'saved'|'error';reason:string;lastError:number;samples:number;required:number;enrolling:string;faces:number;members:IdentityMember[]}
export function isIdentityState(value:unknown):value is IdentityState {
 const s=value as IdentityState|null;
 return !!s&&Number.isSafeInteger(s.revision)&&s.revision>=0&&['idle','collecting','saved','error'].includes(s.phase)&&typeof s.reason==='string'&&typeof s.enrolling==='string'&&[s.lastError,s.samples,s.required,s.faces].every(Number.isFinite)&&Array.isArray(s.members)&&s.members.length<=16&&s.members.every(m=>typeof m.id==='string'&&typeof m.name==='string'&&typeof m.recent==='boolean'&&Number.isFinite(m.seenAgoMs)&&Number.isFinite(m.confidence));
}
export const validIdentityName=(name:string)=>!!name.trim()&&new TextEncoder().encode(name.trim()).length<=63;
export const IDENTITY_REASONS:Record<string,string>={one_face:'请让画面中只留一张人脸',closer:'请靠近一点，并看向镜头',hold_still:'缓慢转动头部，保持在画面内',face_changed:'画面中的人发生变化，请重新登记',duplicate:'这张脸已有登记，可修改原来的称呼',quality:'画面不够清晰，请重新登记',storage:'保存失败，尚未新增档案',changed:'家人列表已变化，请重新登记',timeout:'未采集到足够画面，请重试',stopped:'识别已停止，本次登记未保存'};
