/* Deploy a device build without publishing an index that references missing
 * assets. Usage: node scripts/deploy-device.mjs http://DEVICE BACKUP_DIRECTORY
 * Requires adb connected to that device. Keeps previous assets for rollback. */
import {mkdirSync,readdirSync,readFileSync,writeFileSync} from 'node:fs';
import {resolve,join,relative,sep} from 'node:path';
import {spawnSync} from 'node:child_process';
import {request as httpRequest} from 'node:http';
import {gzipSync,gunzipSync} from 'node:zlib';

const [address,backupArg]=process.argv.slice(2);
if(!address||!backupArg)throw Error('Usage: node scripts/deploy-device.mjs http://DEVICE BACKUP_DIRECTORY');
const base=new URL(address);
if(base.protocol!=='http:'||base.username||base.password||base.search||base.hash)throw Error('Use the device HTTP origin without credentials or query parameters');
const root=resolve(import.meta.dirname,'../dist-device'),backup=resolve(backupArg);
const walk=dir=>readdirSync(dir,{withFileTypes:true}).flatMap(e=>e.isDirectory()?walk(join(dir,e.name)):[join(dir,e.name)]);
const files=walk(root).map(path=>({path,name:relative(root,path).split(sep).join('/')}));
for(const {name} of files)if(name.split('/').some(part=>part.length>=64||!/^[A-Za-z0-9_-][A-Za-z0-9._-]*$/.test(part)))throw Error(`Invalid device path: ${name}`);
function adb(args){const p=spawnSync('adb',args,{encoding:'utf8',timeout:60000});if(p.error)throw p.error;if(p.status!==0)throw Error(p.stderr||p.stdout||`adb failed: ${args[0]}`);}
function get(name){return new Promise((yes,no)=>{
 const req=httpRequest(new URL(name,base),{headers:{'Accept-Encoding':'gzip'}},response=>{
  const chunks=[];response.on('data',chunk=>chunks.push(chunk));response.on('error',no);response.on('end',()=>{
   if(response.statusCode!==200)return no(Error(`${name}: HTTP ${response.statusCode}`));
   try{const bytes=Buffer.concat(chunks);yes(response.headers['content-encoding']==='gzip'?gunzipSync(bytes):bytes);}catch(e){no(e);}
  });
 });req.setTimeout(15000,()=>req.destroy(Error('Device HTTP timeout')));req.on('error',no);req.end();
});}
async function verify(file){const data=await get(file.name);if(!data.equals(readFileSync(file.path)))throw Error(`HTTP content mismatch: ${file.name}`);console.log(`HTTP 200 verified ${file.name}`);}
mkdirSync(backup,{recursive:true});
const oldIndex=await get('index.html');
writeFileSync(join(backup,'index.html'),oldIndex,{flag:'wx'});
writeFileSync(join(backup,'index.html.gz'),gzipSync(oldIndex),{flag:'wx'});
const assets=files.filter(f=>f.name!=='index.html'&&f.name!=='index.html.gz');
const dirs=[...new Set(assets.map(f=>f.name.split('/').slice(0,-1).join('/')).filter(Boolean))].sort();
for(const dir of dirs)adb(['shell',`mkdir -p /data/www/${dir}`]);
for(const file of assets)adb(['push',file.path,`/data/www/${file.name}`]);
for(const file of assets.filter(f=>!f.name.endsWith('.gz')))await verify(file);
// Only publish the entry point once every new dependency is actually served.
for(const name of ['index.html.gz','index.html'])adb(['push',join(root,name),`/data/www/${name}`]);
await verify({name:'index.html',path:join(root,'index.html')});
console.log(`Device panel deployed. Previous entry point: ${backup}`);
