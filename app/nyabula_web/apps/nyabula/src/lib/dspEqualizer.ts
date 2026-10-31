export type EqType = 'peak' | 'lowShelf' | 'highShelf';
export interface EqBand { frequencyHz:number; q:number; gainDb:number; enabled:boolean; type:EqType }
export const EQ_FREQUENCIES = [31.5,50,80,125,200,300,500,800,1200,2000,3150,4500,6000,9000,12500,16000];
export const LEGACY_BANDS = [2,5,8,12];
export function flatBands():EqBand[] { return EQ_FREQUENCIES.map(frequencyHz => ({ frequencyHz, q:1.4, gainDb:0, enabled:true, type:'peak' })); }
export function eqCoefficients(b:EqBand):number[] {
  if (!b.enabled || b.gainDb === 0) return [1,0,0,1,0,0];
  const w=2*Math.PI*b.frequencyHz/48000, c=Math.cos(w), a=10**(b.gainDb/40), alpha=Math.sin(w)/(2*b.q), beta=2*Math.sqrt(a)*alpha;
  if (b.type==='lowShelf') return [a*((a+1)-(a-1)*c+beta),2*a*((a-1)-(a+1)*c),a*((a+1)-(a-1)*c-beta),(a+1)+(a-1)*c+beta,-2*((a-1)+(a+1)*c),(a+1)+(a-1)*c-beta];
  if (b.type==='highShelf') return [a*((a+1)+(a-1)*c+beta),-2*a*((a-1)+(a+1)*c),a*((a+1)+(a-1)*c-beta),(a+1)-(a-1)*c+beta,2*((a-1)-(a+1)*c),(a+1)-(a-1)*c-beta];
  return [1+alpha*a,-2*c,1-alpha*a,1+alpha/a,-2*c,1-alpha/a];
}
export function bandResponse(b:EqBand, hz:number):number {
  const [b0,b1,b2,a0,a1,a2]=eqCoefficients(b), w=2*Math.PI*hz/48000;
  const power=(v0:number,v1:number,v2:number)=>(v0+v1*Math.cos(w)+v2*Math.cos(2*w))**2+(v1*Math.sin(w)+v2*Math.sin(2*w))**2;
  return 10*Math.log10(power(b0!,b1!,b2!)/power(a0!,a1!,a2!));
}
export function eqResponse(bands:EqBand[], hz:number):number { return bands.reduce((db,b)=>db+bandResponse(b,hz),0); }
export function eqPeak(bands:EqBand[]):number {
  const frequencies=Array.from({length:801},(_,i)=>20*1000**(i/800));
  frequencies.push(...bands.map(b=>b.frequencyHz));
  return Math.max(0,...frequencies.map(f=>eqResponse(bands,f)));
}
export function validBands(bands:EqBand[]):boolean {
  return bands.length===16 && bands.every(b => Number.isFinite(b.frequencyHz)&&b.frequencyHz>=20&&b.frequencyHz<=18000&&Number.isFinite(b.q)&&b.q>=.2&&b.q<=10&&Number.isFinite(b.gainDb)&&Math.abs(b.gainDb)<=12&&typeof b.enabled==='boolean'&&['peak','lowShelf','highShelf'].includes(b.type));
}
export const LISTENING_PRESETS = [
  {id:'rich', name:'丰富', detail:'低频饱满 · 高频明亮'},
  {id:'balanced', name:'平衡', detail:'中频清晰 · 轻微修饰'},
  {id:'mild', name:'温和', detail:'厚实柔和 · 收敛高频'},
] as const;
const PRESET_GAINS:Record<string,number[]> = {
  rich:[3,3,2.5,2,1,0,-1,-1.5,-1,-.5,.5,1.5,2,2,1.5,1],
  balanced:[0,0,0,.2,.3,.5,.6,.8,1,1.2,1,.5,0,-.2,-.5,-.5],
  mild:[1,1.5,2,2,1.5,.8,0,-.5,-1,-1.5,-2,-2.5,-3,-3.5,-4,-4.5],
};
export function eqPreset(name:string):EqBand[] {
  const bands=flatBands();
  const gains=PRESET_GAINS[name];
  if(gains) return bands.map((b,i)=>({...b,gainDb:gains[i]!}));
  if(name==='warm') { bands[2]={frequencyHz:140,q:.707,gainDb:3,enabled:true,type:'lowShelf'};bands[5]!.gainDb=-1;bands[12]={frequencyHz:6500,q:.707,gainDb:-2,enabled:true,type:'highShelf'}; }
  if(name==='voice') { bands[2]={frequencyHz:140,q:.707,gainDb:-3,enabled:true,type:'lowShelf'};bands[5]!.gainDb=-2;bands[8]={frequencyHz:2500,q:.9,gainDb:2.5,enabled:true,type:'peak'}; }
  if(name==='soft') { bands[8]={frequencyHz:3200,q:1.2,gainDb:-2,enabled:true,type:'peak'};bands[12]={frequencyHz:7000,q:.707,gainDb:-3,enabled:true,type:'highShelf'}; }
  return bands;
}
// Equal-log-frequency energy estimates a common pink-noise level reference.
// Actual musical loudness still depends on its spectrum.
export function presetMasterDb(bands:EqBand[]):number {
  const powers=Array.from({length:321},(_,i)=>10**(eqResponse(bands,31.5*(16000/31.5)**(i/320))/10));
  const energyDb=10*Math.log10(powers.reduce((a,b)=>a+b,0)/powers.length);
  return Math.max(-30,Math.min(-6,-Math.ceil((6+energyDb)*2)/2,-Math.ceil(eqPeak(bands)+1)));
}
export function expandLegacyBands(bands?:EqBand[],gains:number[]=[]):EqBand[] {
  if(bands?.length===16)return bands.map(b=>({...b}));
  const result=flatBands();
  LEGACY_BANDS.forEach((target,i)=>{result[target]=bands?.[i]?{...bands[i]!}:{frequencyHz:[80,300,1200,6000][i]!,q:1,gainDb:gains[i]??0,enabled:true,type:'peak'};});
  return result;
}
