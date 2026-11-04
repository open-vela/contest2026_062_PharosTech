// Object tracking remains experimental until real-model validation passes.
export const OBJECT_TRACKING_RELEASED = false;
export interface VisionFace { x:number;y:number;width:number;height:number;confidence:number;landmarks:number[] }
export interface VisionState {running:boolean;phase:'idle'|'loading'|'detecting'|'learning'|'tracking'|'lost'|'error';following:boolean;identifying?:boolean;tracking?:boolean;frames:number;lastError:number;followError:number;ageMs?:number;result?:{sequence:number;capturedMs:number;faces:VisionFace[];target?:Omit<VisionFace,'landmarks'>;progress?:number;views?:number;phase?:string}}
export function isVisionState(value:unknown):value is VisionState {
 const s=value as VisionState|null;
 if(!s||typeof s.running!=='boolean'||typeof s.following!=='boolean'||!['idle','loading','detecting','learning','tracking','lost','error'].includes(s.phase)||![s.frames,s.lastError,s.followError].every(Number.isFinite))return false;
 if(s.identifying!==undefined&&typeof s.identifying!=='boolean')return false;
 if(s.tracking!==undefined&&typeof s.tracking!=='boolean')return false;
 if(s.result===undefined)return true;
 if(s.tracking){const t=s.result.target;if(!t||![t.x,t.y,t.width,t.height,t.confidence].every(n=>Number.isFinite(n)&&n>=0&&n<=1)||!Number.isFinite(s.result.progress)||s.result.progress! < 0||s.result.progress! > 100)return false;}

 return Number.isFinite(s.ageMs)&&(s.ageMs ?? -1)>=0&&Number.isSafeInteger(s.result.sequence)&&Array.isArray(s.result.faces)&&s.result.faces.length<=5&&s.result.faces.every(f=>
  [f.x,f.y,f.width,f.height,f.confidence].every(n=>Number.isFinite(n)&&n>=0&&n<=1)&&Array.isArray(f.landmarks)&&f.landmarks.length===10&&f.landmarks.every(Number.isFinite));
}
