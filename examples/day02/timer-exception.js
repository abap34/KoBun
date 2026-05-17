setTimeout(function () {
    console.log("timer:before throw");
    throw new Error("timer failed");
}, 0);

console.log("script:scheduled");
