<?php
// One new native connection per request, after FPM has forked.
error_log('version='.DATAROUTER_GIT_COMMIT);
header('Content-Type: application/json');
$p = null;
try {
    $p = datarouter_open(getenv('DR_SHM'), 'Php'.getmypid(), (int)(getenv('DR_PLANE') ?: DATAROUTER_DIRECT));
    $wire = datarouter_pack(json_encode(['action'=>'Public.Products.List', 'rid'=>$_SERVER['QUERY_STRING']], JSON_THROW_ON_ERROR));
    datarouter_send($p, 'core', $wire);
    $reply = datarouter_recv($p);
    if ($reply['source'] !== 'core' || $reply['data'] !== $wire) throw new RuntimeException('reply mismatch');
    echo json_encode(['pid'=>getmypid(), 'rid'=>$_SERVER['QUERY_STRING'], 'errors'=>0, 'version'=>DATAROUTER_GIT_COMMIT, 'php'=>PHP_VERSION], JSON_THROW_ON_ERROR);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode(['errors'=>1, 'code'=>$e->getCode(), 'message'=>$e->getMessage()]);
} finally {
    if ($p !== null) datarouter_close($p);
}
