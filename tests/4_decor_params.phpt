--TEST--
Decorators: Parameters of decorators
--SKIPIF--
<?php require_once(__DIR__.'/skipif.inc'); ?>
--FILE--
<?php
function replacer($func, $name)
{
    return function () use($func, $name)
    {
        return call_user_func_array($name, func_get_args());
    };
}

function quiet($func)
{
    return function () use($func)
    {
        $er = error_reporting(0);
        $res = call_user_func_array($func, func_get_args());
        error_reporting($er);
        return $res;
    };
}

function wlog($func, $logger, $str)
{
    return function () use($func, $logger, $str)
    {
        call_user_func($logger, $str);
        return call_user_func_array($func, func_get_args());
    };
}

function trigger($func, $when, $what, $params=array())
{
    return function () use($func, $when, $what, $params)
    {
        if (!strcasecmp($when, 'before')) {
            call_user_func_array($what, $params);
        }
        $res = call_user_func_array($func, func_get_args());
        if (!strcasecmp($when, 'after')) {
            call_user_func_array($what, $params);
        }
        return $res;
    };
}

class Logger
{
    public static function info($str)
    {
        printf("%s: %s\n", __METHOD__, $str);
    }
}

# @replacer(function(){return microtime(true);})
function my_time()
{
    throw new RuntimeException('Decorators is not working');
}

# @wlog('Logger::info', 'failer() was called')
# @quiet
function failer()
{
    return fopen('/wrong/path/to/nothing', 'rb');
}

function TollDerSpion($ns, $DasGestohleneGeheimnis)
{
    var_dump($ns.': '.$DasGestohleneGeheimnis);
}

#@trigger('before', 'TollDerSpion', array('stolen', $secret))
function uberFunc($secret)
{
    $secret = null;
}

var_dump(my_time() !== null); // float(12345.67890)

var_dump(failer());

uberFunc('uber_secret_value');
?>
--EXPECTF--
bool(true)
Logger::info: failer() was called
bool(false)
string(25) "stolen: uber_secret_value"

