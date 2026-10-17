import { describe, it, expect } from 'vitest';
import { EyeEngine } from '../src/engine.js';

function setup(toyMode=true) {
  const handlers=new Map<string,(e:any)=>void>();
  const events: unknown[]=[];
  const canvas={getContext:()=>null, getBoundingClientRect:()=>({left:100,top:50,width:800,height:480}),
    addEventListener:(n:string,cb:any)=>handlers.set(n,cb), removeEventListener:(n:string)=>handlers.delete(n),
    setPointerCapture:()=>{},hasPointerCapture:()=>false};
  const engine=new EyeEngine(canvas as unknown as HTMLCanvasElement, {toyMode,onInteraction:(...args)=>events.push(args)});
  const send=(name:string,patch={})=>handlers.get(name)?.({pointerId:1,pointerType:'mouse',isPrimary:true,button:0,clientX:900,clientY:50,...patch});
  return {engine,events,handlers,send};
}
describe('cat toy input',()=>{
  it('tracks mouse hover only when enabled and normalizes canvas-local coordinates',()=>{
    const {engine,send,events}=setup(); send('pointermove');
    expect(events).toEqual([[1,-0.92,'move']]);
    engine.setToyMode(false); send('pointermove'); expect(events).toHaveLength(1);
    engine.destroy();
  });
  it('captures one touch, ignores a second finger, and releases on cancellation',()=>{
    const {engine,send,events,handlers}=setup();
    send('pointermove',{pointerType:'touch'}); expect(events).toHaveLength(0);
    send('pointerdown',{pointerType:'touch'});
    send('pointerdown',{pointerId:2,pointerType:'touch',isPrimary:false});
    send('pointermove',{pointerId:2,pointerType:'touch'}); expect(events).toHaveLength(1);
    send('pointercancel',{pointerType:'touch'}); expect(events).toHaveLength(2);
    send('lostpointercapture'); expect(events).toHaveLength(2);
    engine.destroy(); expect(handlers.size).toBe(0);
  });
});
