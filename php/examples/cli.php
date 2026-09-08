<?php
// CGSGL v2. Run via cgeos-cli datarouter php verify.
fwrite(STDERR, 'version='.DATAROUTER_GIT_COMMIT."\n");
[$script, $shm, $role, $count, $plane] = $argv;
$p = datarouter_open($shm, $role === 'server' ? 'core' : 'portal', (int)$plane);
try {
    if ($role === 'server') {
        for ($i = 0; $i < (int)$count; $i++) {
            $m = datarouter_recv($p, 10000);
            datarouter_send($p, $m['source'], $m['data'], 10000);
        }
    } else {
        for ($i = 0; $i < (int)$count; $i++) {
            $body = json_encode(['action'=>'Public.Products.List', 'rid'=>$i], JSON_THROW_ON_ERROR);
            $wire = datarouter_pack($body);
            datarouter_send($p, 'core', $wire);
            $m = datarouter_recv($p);
            if ($m['data'] !== $wire || $m['source'] !== 'core') throw new RuntimeException('echo mismatch');
        }
    }
    echo json_encode(['count'=>(int)$count, 'errors'=>0], JSON_THROW_ON_ERROR)."\n";
} finally { datarouter_close($p); }
