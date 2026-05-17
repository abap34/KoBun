console.log("script:start");

queueMicrotask(function () {
    console.log("microtask:queued");
});

Promise.resolve().then(function () {
    console.log("microtask:promise");
});

setTimeout(function () {
    console.log("timer");
}, 0);

console.log("script:end");
