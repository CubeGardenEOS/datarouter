<?php
fwrite(STDERR, 'version='.DATAROUTER_GIT_COMMIT."\n");
[$script,$shm,$dir]=$argv;
function awaitMarker($path) { $end=microtime(true)+15; while (!file_exists($path)) { if (microtime(true)>$end) throw new RuntimeException('marker timeout '.$path); usleep(1000); } }
$p=datarouter_open($shm,'portal');
file_put_contents($dir.'/ready','1');
awaitMarker($dir.'/down');
try { datarouter_send($p,'core',datarouter_pack('x'),100); throw new RuntimeException('missing peer accepted'); }
catch (DataRouterException $e) { if ($e->getCode()!==2) throw $e; }
datarouter_close($p);
file_put_contents($dir.'/closed','1');
awaitMarker($dir.'/up');
$p=datarouter_open($shm,'portal'); $b=datarouter_open($shm,'core');
$wire=datarouter_pack("recovered\0"); datarouter_send($p,'core',$wire);
$m=datarouter_recv($b);
if ($m['data']!==$wire || $m['source']!=='portal') throw new RuntimeException('recovery mismatch');
datarouter_close($p);datarouter_close($b);
file_put_contents($dir.'/result.json',json_encode(['case'=>'restart','checks'=>2,'errors'=>0],JSON_THROW_ON_ERROR));
