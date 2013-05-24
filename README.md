php-decorators
==============

The implementation of decorators in PHP

###Usage example

```php
<?php
class Logger
{
    public static function log($func, $text='')
    {
        return function() use($func, $text) {
            printf("Log: %s\n", $text);
            return call_user_func_array($func, func_get_args());
        };
    }
}

function add($func, $v=0) {
    return function() use($func, $v) {
        return $v + call_user_func_array($func, func_get_args());
    };
}

@Logger::log('calling b()')
@add(41)
function b()
{
    return 1;
}

var_dump(b());
```
Returns:
<pre>
Log: calling b()
int(42)
</pre>
