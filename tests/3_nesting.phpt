--TEST--
Decorators: Decorators nesting
--SKIPIF--
<?php require_once(__DIR__.'/skipif.inc'); ?>
--FILE--
<?php
function add($func, $v=0)
{
    return function () use($func, $v)
    {
        return call_user_func_array($func, func_get_args()) + $v;
    };
}

function mul($func, $v=0)
{
    return function () use($func, $v)
    {
        return call_user_func_array($func, func_get_args()) * $v;
    };
}

function neg($func)
{
    return function () use($func)
    {
        return -call_user_func_array($func, func_get_args());
    };
}

#@neg
#@mul(6)
#@add(6)
function one()
{
    return 1;
}

var_dump(one());
?>
--EXPECTF--
int(-42)
