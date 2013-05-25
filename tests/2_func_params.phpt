--TEST--
Decorators: Parameters of decorated functions
--SKIPIF--
<?php require_once(__DIR__.'/skipif.inc'); ?>
--FILE--
<?php
function noth($func)
{
    return function () use($func)
    {
        return call_user_func_array($func, func_get_args());
    };
}

#@noth
function a($v=0)
{
    return $v;
}

var_dump(a());
var_dump(a(1));

#@noth
function b(&$v)
{
    $v *= 2;
    return $v;
}

$x = 21;
var_dump($x);
var_dump(b($x)); // pass-by-ref not supported
var_dump($x);
?>
--EXPECTF--
int(0)
int(1)
int(21)
int(42)
int(21)
