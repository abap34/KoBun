const res = new Response("ok", {
  headers: {
    "content-type": "text/plain",
    "x-kobun": "day3",
  },
});

console.log("content-type", res.headers["content-type"]);
console.log("x-kobun", res.headers["x-kobun"]);

const empty = new Response("ok");
console.log("default headers object", typeof empty.headers);
console.log("missing header", empty.headers["content-type"] === undefined);
