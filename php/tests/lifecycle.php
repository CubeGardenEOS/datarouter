<?php
fwrite(STDERR,'version='.DATAROUTER_GIT_COMMIT."\n");
[$script,$shm,$mode,$ready]=$argv;
$p=datarouter_open($shm,'core');
file_put_contents($ready,'1');
if ($mode==='success') exit(0);
if ($mode==='failure') exit(42);
sleep(30); // driver enforces timeout / interruption and reaps this process
