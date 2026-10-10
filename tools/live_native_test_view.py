"""Local read-only viewer of captures from the current native diagnostic.
Only named diagnostic BMPs and summarized log lines are served on loopback.
"""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import json,re,os,sys
ROOT=Path(__file__).resolve().parents[1]
ANALYSIS=ROOT/'analysis'
PREFIX='native_performance_vulkan_cached_batch_20261005_'
HTML=r"""<!doctype html><meta charset="utf-8"><title>K-On! — prueba en curso</title><style>
body{margin:0;background:#14121c;color:#eee;font:16px system-ui}main{max-width:1100px;margin:24px auto;padding:0 20px}h1{font-size:24px}#screen{position:relative}#fps{display:inline-block;margin-top:10px;background:#090d12df;color:#78ffb4;padding:8px 12px;border-radius:6px;font-weight:700}img{display:block;width:100%;aspect-ratio:480/272;object-fit:contain;background:black;image-rendering:auto;border-radius:12px}p{color:#cdc6dc}pre{white-space:pre-wrap;background:#252031;padding:16px;border-radius:10px;font-size:14px}.pill{color:#8bebbc}small{color:#b6adcb}</style><main><h1>K-On! · prueba del juego original</h1><p class=pill id=phase>Buscando captura…</p><div id=screen><img id=frame><span id=fps>FPS juego: --</span></div><p>Vista de diagnóstico: se actualiza aproximadamente una vez por segundo. Esas actualizaciones no son los FPS del juego.</p><pre id=stats></pre><small>Entradas automáticas · copia privada de la partida · prueba incompleta</small></main><script>
async function refresh(){try{let data=await(await fetch('/state',{cache:'no-store'})).json();document.querySelector('#phase').textContent=data.phase;document.querySelector('#stats').textContent=data.progress;document.querySelector('#fps').textContent=data.fps===null?'FPS juego: esperando muestra':'FPS juego: '+data.fps.toFixed(1)+' · últimos '+data.fps_seconds.toFixed(1)+' s';if(data.image)document.querySelector('#frame').src='/frame?stamp='+data.stamp}catch(e){document.querySelector('#phase').textContent='Esperando capturas de la prueba…'}}refresh();setInterval(refresh,1000);
</script>"""
def snapshot():
 logs=[p for p in ANALYSIS.glob('native_*_en.log') if p.name.startswith(PREFIX) or 'visible' in p.stem or 'frame_replay' in p.stem or 'vulkan_resident' in p.stem]
 if not logs:return None,'Esperando prueba','',None,None,None
 log=max(logs,key=lambda p:p.stat().st_mtime_ns)
 with log.open('rb') as stream:
  stream.seek(max(0,log.stat().st_size-2097152));text=stream.read().decode('utf-8-sig',errors='replace')
 lines=re.findall(r'^native_progress[^\n]*',text,re.M)
 if not lines and ('visible' in log.stem or 'Registro: ' in text):
  redirected=ANALYSIS/'native_interactive_en.log'
  if redirected.exists():
   with redirected.open('rb') as stream:
    stream.seek(max(0,redirected.stat().st_size-2097152));redirected_text=stream.read().decode('utf-8-sig',errors='replace')
   lines=re.findall(r'^native_progress[^\n]*',redirected_text,re.M)
 images=list(ANALYSIS.glob(log.stem+'_*.bmp'))
 if images:
  # Numeric elapsed time, not lexical file order.
  image=max(images,key=lambda p:int(re.search(r'_(\d+)ms.bmp$',p.name)[1]))
 else:image=log.with_suffix('.bmp') if log.with_suffix('.bmp').exists() else None
 finished='process_exit_code=' in text
 phase=log.stem.replace('native_performance_','')+(' · terminó' if finished else ' · en ejecución')
 fps=None;seconds=None
 if len(lines)>=2:
  samples=[(int(re.search(r'elapsed_ms=(\d+)',line)[1]),int(re.search(r'submitted_frames=(\d+)',line)[1])) for line in lines[-2:]]
  seconds=(samples[1][0]-samples[0][0])/1000
  if seconds>0:fps=(samples[1][1]-samples[0][1])/seconds
 return image,phase,lines[-1] if lines else 'Cargando el juego…',image.stat().st_mtime_ns if image else None,fps,seconds
class Handler(BaseHTTPRequestHandler):
 def do_GET(self):
  path=self.path.split('?',1)[0]
  if path=='/':content=HTML.encode();mime='text/html; charset=utf-8'
  elif path=='/state':
   image,phase,progress,stamp,fps,seconds=snapshot();content=json.dumps({'phase':phase,'progress':progress,'image':bool(image),'stamp':stamp,'fps':fps,'fps_seconds':seconds}).encode();mime='application/json'
  elif path=='/frame':
   image,_,_,_,_,_=snapshot()
   if image is None:self.send_error(404);return
   content=image.read_bytes();mime='image/bmp'
  else:self.send_error(404);return
  self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(content)));self.end_headers();self.wfile.write(content)
 def log_message(self,*args):pass
ThreadingHTTPServer.allow_reuse_address=True
server=ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1]) if len(sys.argv)>1 else 0),Handler)
port=server.server_address[1]
(ROOT/'analysis/native_live_viewer.json').write_text(json.dumps({'url':f'http://127.0.0.1:{port}/','capture_cadence_ms':1000,'read_only':True,'pid':os.getpid()})+'\n',encoding='utf-8')
print(f'Live diagnostic view: http://127.0.0.1:{port}/',flush=True)
server.serve_forever()
