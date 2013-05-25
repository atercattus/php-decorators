--TEST--
Decorators: testing of module
--SKIPIF--
<?php require_once(__DIR__.'/skipif.inc'); ?>
--FILE--
<?php
var_dump(decorators_preprocessor('echo 1;'));
?>
--EXPECTF--
string(7) "echo 1;"
