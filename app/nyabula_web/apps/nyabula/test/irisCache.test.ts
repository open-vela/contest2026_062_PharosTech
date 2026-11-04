import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { readIrisCache, rememberIris } from '../src/lib/irisCache';
beforeEach(()=>{
 const values=new Map<string,string>();
 vi.stubGlobal('localStorage',{getItem:vi.fn((key:string)=>values.get(key)??null),setItem:vi.fn((key:string,value:string)=>values.set(key,value))});
});
afterEach(()=>{vi.restoreAllMocks();vi.unstubAllGlobals();});
it('keeps confirmed per-eye colours separate for each device',()=>{
 rememberIris('self',{left:'#6FD3FF',right:'#FF80AA'});
 rememberIris('other',{left:'#112233',right:'#445566'});
 expect(readIrisCache('self')).toEqual({left:'#6fd3ff',right:'#ff80aa'});
 expect(readIrisCache('other')).toEqual({left:'#112233',right:'#445566'});
 expect(readIrisCache('new')).toBeNull();
});
it('uses the uncached path for malformed values or unavailable storage',()=>{
 localStorage.setItem('nyabula:iris:self','{"left":"bad","right":"#ffffff"}');
 expect(readIrisCache('self')).toBeNull();
 vi.spyOn(localStorage,'getItem').mockImplementation(()=>{throw Error('disabled');});
 expect(readIrisCache('self')).toBeNull();
 expect(()=>rememberIris('self',{left:'#112233',right:'#445566'})).not.toThrow();
});
it('does not rewrite unchanged values on every eye-state event',()=>{
 const write=vi.spyOn(localStorage,'setItem');
 rememberIris('self',{left:'#112233',right:'#445566'});
 rememberIris('self',{left:'#112233',right:'#445566'});
 expect(write).toHaveBeenCalledTimes(1);
});
