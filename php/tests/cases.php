<?php
fwrite(STDERR, 'version='.DATAROUTER_GIT_COMMIT."\n");
[$script, $shm, $case, $out] = $argv;
$checks = 0;
function check($condition) { global $checks; if (!$condition) throw new RuntimeException('check '.($checks+1).' failed'); $checks++; }
function errorCode($code, $fn) { try { $fn(); } catch (DataRouterException $e) { check($e->getCode() === $code); return; } throw new RuntimeException('expected error '.$code); }
if ($case === 'auth') {
    errorCode(8, fn()=>datarouter_open($shm, 'portal', 0, 100, 'wrong'));
    $p = datarouter_open($shm, 'portal', 0, 1000, 'secret'); datarouter_close($p); check(true);
} elseif ($case === 'basic') {
    $a = datarouter_open($shm, 'portal'); $b = datarouter_open($shm, 'core');
    foreach (['', "a\0b\xff", str_repeat("\0\xffA", 5000)] as $body) {
        $wire = datarouter_pack($body); check(datarouter_unpack($wire)['body'] === $body);
        datarouter_send($a, 'core', $wire); $m = datarouter_recv($b); check($m['source'] === 'portal' && $m['data'] === $wire);
        datarouter_send($b, 'portal', $wire); check(datarouter_recv($a)['data'] === $wire);
    }
    // Alternating capacities and retained return values must never share storage.
    $saved = []; $expected = [];
    foreach ([0,4087,4088,4089,307200,1,32768,0,307200,3] as $n) {
        $body = str_repeat("\0", $n); $wire = datarouter_pack($body);
        datarouter_send($a, 'core', $wire); $saved[] = datarouter_recv($b)['data']; $expected[] = $wire;
        check(end($saved) === $wire); check(datarouter_unpack(end($saved))['body'] === $body);
    }
    foreach ($saved as $i=>$value) check($value === $expected[$i]);
    foreach (['', "\x07", str_repeat("\0",7), "\x07\0\0\0\x01\0\0\0",
              "\x07\0\0\0\0\0\0\0X", "\x07\0\x01\0\0\0\0\0X",
              "\x07\0\x03\0\0\0\0\0\x10\xff\xff",
              "\x07\0\x04\0\0\0\0\0\x10\x02\0X"] as $bad) {
        errorCode(10, fn()=>datarouter_unpack($bad));
    }
    $wire = "\xfe\xa5\x05\0\x03\0\0\0\x10\x02\0\0Xz\0q";
    check(datarouter_unpack($wire) === ['type'=>254,'flags'=>165,'body'=>"z\0q",'var_data'=>"\x10\x02\0\0X"]);
    $max = datarouter_max_payload(); check($max > 300*1024);
    foreach ([$max-1, $max] as $n) {
        $wire = datarouter_pack(str_repeat('x', $n-8)); check(strlen($wire) === $n);
        datarouter_send($a, 'core', $wire); check(datarouter_recv($b)['data'] === $wire);
    }
    errorCode(5, fn()=>datarouter_send($a, 'core', datarouter_pack(str_repeat('x', $max-7))));
    errorCode(7, fn()=>datarouter_recv($a, 20));
    errorCode(2, fn()=>datarouter_send($a, 'Missing', datarouter_pack('x'), 20));
    errorCode(10, fn()=>datarouter_unpack("bad\0"));
    errorCode(10, fn()=>datarouter_pack('x', 7, "\x10\xff\xff"));
    $wire = datarouter_pack(str_repeat('x', 300*1024));
    datarouter_send($a, 'core', $wire);
    errorCode(4, fn()=>datarouter_send($a, 'core', $wire, 0));
    check(datarouter_recv($b)['data'] === $wire);
    datarouter_close($b); datarouter_close($b); check(true);
    errorCode(1, fn()=>datarouter_recv($b));
    errorCode(1, fn()=>datarouter_send($b, 'portal', $wire));
    errorCode(2, fn()=>datarouter_send($a, 'core', $wire));
    $b = datarouter_open($shm, 'core'); datarouter_send($a, 'core', datarouter_pack('recovered'));
    check(datarouter_unpack(datarouter_recv($b)['data'])['body'] === 'recovered');
    datarouter_close($a); datarouter_close($b);
} elseif ($case === 'routed') {
    $p = datarouter_open($shm, 'portal', 1);
    foreach ([datarouter_max_payload()-8, datarouter_max_payload()+8192] as $n) {
        $wire = datarouter_pack(str_repeat("\0", $n));
        datarouter_send($p, 'core', $wire, 10000); $m = datarouter_recv($p, 10000);
        check($m['source'] === 'core' && $m['data'] === $wire);
    }
    datarouter_close($p);
} else { throw new RuntimeException('unknown case'); }
file_put_contents($out, json_encode(['case'=>$case, 'checks'=>$checks, 'errors'=>0], JSON_THROW_ON_ERROR));
