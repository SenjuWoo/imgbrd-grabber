import { search, makeGrabber } from "../test-utils";
import { source } from "./model";

// Markup and payloads below are trimmed copies of what gelbooru.com, safebooru.org and api.rule34.xxx answer.
describe("Gelbooru (0.2)", () => {
    beforeAll(makeGrabber);

    describe("JSON API", () => {
        const parse = (src: string) => source.apis.json.search.parse(src, 200) as any;

        it("builds the search URL", () => {
            expect(search(source.apis.json, "tag", 2)).toEqual("/index.php?page=dapi&s=post&q=index&limit=10&pid=1&tags=tag&json=1");
        });

        it("reads the real MD5 from the hash field, not from the file name", () => {
            const res = parse(JSON.stringify([{
                id: 7303945,
                hash: "4f448814506bd10298188b7fa8c1e463",
                image: "9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.jpg",
                file_url: "https://safebooru.org/images/1112/9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.jpg",
                preview_url: "https://safebooru.org/thumbnails/1112/thumbnail_9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.jpg",
                owner: "someone",
            }]));
            expect(res.images.length).toEqual(1);
            expect(res.images[0].md5).toEqual("4f448814506bd10298188b7fa8c1e463");
        });

        it("treats the empty body of a search without results as an empty page", () => {
            const res = parse("");
            expect(res.error).toBeUndefined();
            expect(res.images).toEqual([]);
        });

        it("reports an error object instead of throwing", () => {
            expect(parse(JSON.stringify({ success: false, message: "Missing authentication" })).error).toContain("Missing authentication");
            expect(parse("\"Missing authentication. Go to api.rule34.xxx for more information\"").error).toContain("Missing authentication");
        });

        it("reports a body that is not JSON instead of throwing", () => {
            expect(parse("<html>Too many requests</html>").error).toBeDefined();
        });

        describe("Details", () => {
            const details = (src: string) => source.apis.json.details!.parse(src, 200) as any;

            it("reports the refusal of an anonymous request instead of throwing", () => {
                // api.rule34.xxx answers HTTP 200 with a bare JSON string
                const res = details("\"Missing authentication. Go to api.rule34.xxx for more information\"");
                expect(res.error).toMatch(/^Missing authentication/);
                expect(details(JSON.stringify({ success: false, message: "Missing authentication" })).error).toMatch(/^Missing authentication/);
                expect(details("").error).toBeDefined();
                expect(details("<html>Too many requests</html>").error).toBeDefined();
                expect(details(JSON.stringify({ "@attributes": { count: 0 } })).error).toBeDefined();
            });

            it("gives the real file address and the tags", () => {
                const res = details(JSON.stringify({
                    "@attributes": { limit: 100, offset: 0, count: 1 },
                    post: [{
                        id: 5089291,
                        hash: "e9586f76c9438fafd806e987aedb799b",
                        image: "e9586f76c9438fafd806e987aedb799b.png",
                        file_url: "https://wimg.rule34.xxx/images/4468/e9586f76c9438fafd806e987aedb799b.png",
                        preview_url: "https://wimg.rule34.xxx/thumbnails/4468/thumbnail_e9586f76c9438fafd806e987aedb799b.jpg",
                        source: "https://example.com/post/1",
                        created_at: "Sat Nov 08 12:34:56 -0600 2025",
                        tags: "landscape  sky",
                        tag_info: [{ tag: "landscape", type: "tag", count: 10 }, { tag: "someone", type: "artist", count: 2 }],
                    }],
                }));
                expect(res.error).toBeUndefined();
                expect(res.imageUrl).toEqual("https://wimg.rule34.xxx/images/4468/e9586f76c9438fafd806e987aedb799b.png");
                expect(res.source).toEqual("https://example.com/post/1");
                expect(res.createdAt).toEqual("Sat Nov 08 12:34:56 -0600 2025");
                expect(res.tags).toEqual([{ name: "landscape", type: "general", count: 10 }, { name: "someone", type: "artist", count: 2 }]);
            });

            it("does not offer an address guessed from the thumbnail", () => {
                const res = details(JSON.stringify([{ id: 1, tags: "a b", preview_url: "https://x.test/thumbnails/1/thumbnail_abc.jpg" }]));
                expect(res.imageUrl).toBeUndefined();
                expect(res.tags).toEqual(["a", "b"]);
            });
        });

        it("accepts the wrapped format without posts", () => {
            const res = parse(JSON.stringify({ "@attributes": { limit: 10, offset: 0, count: 0 } }));
            expect(res.error).toBeUndefined();
            expect(res.images).toEqual([]);
            expect(res.imageCount).toEqual(0);
        });
    });

    describe("Regex API", () => {
        const listing = (host: string, hash: string) => '<span id="s7303945" class="thumb"><a id="p7303945" href="index.php?page=post&amp;s=view&amp;id=7303945">'
            + '<img src="https://' + host + '/thumbnails/1112/thumbnail_' + hash + '.jpg?7303945" title=" landscape sky  score:0 rating:general" border="0" alt="landscape sky"/></a></span>';

        it("keeps a 32-character thumbnail name as the MD5", () => {
            const res = source.apis.html.search.parse(listing("img4.gelbooru.com", "4f448814506bd10298188b7fa8c1e463"), 200) as any;
            expect(res.images.length).toEqual(1);
            expect(res.images[0].md5).toEqual("4f448814506bd10298188b7fa8c1e463");
        });

        it("does not mistake a 40-character file hash for the MD5", () => {
            const res = source.apis.html.search.parse(listing("safebooru.org", "9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe"), 200) as any;
            expect(res.images.length).toEqual(1);
            expect(res.images[0].md5).toBeUndefined();
            expect(res.images[0].file_url).toEqual("https://safebooru.org/images/1112/9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.jpg?7303945");
        });

        describe("Details", () => {
            const parse = (src: string) => source.apis.html.details!.parse(src, 200) as any;

            it("finds the original on a gelbooru.com post page", () => {
                const res = parse('<meta property="og:image" content="https://img4.gelbooru.com//images/4f/44/4f448814506bd10298188b7fa8c1e463.png" />'
                    + '<img width="850" height="1284" id="image" class="fit-width" src="https://img4.gelbooru.com//samples/4f/44/sample_4f448814506bd10298188b7fa8c1e463.jpg">'
                    + '<li><a href="https://img4.gelbooru.com/images/4f/44/4f448814506bd10298188b7fa8c1e463.png" target="_blank" rel="noopener" style="font-weight: bold;">Original image</a></li>');
                expect(res.imageUrl).toEqual("https://img4.gelbooru.com/images/4f/44/4f448814506bd10298188b7fa8c1e463.png");
            });

            it("finds the original on a safebooru.org or rule34.xxx post page", () => {
                const res = parse('<meta property="og:image" itemprop="image" content="https://safebooru.org//images/1112/9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.png?7303945" />'
                    + '<img alt="landscape" src="https://safebooru.org//samples/1112/sample_9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.jpg?7303945" id="image" onclick="if (!window.__cfRLUnblockHandlers) return false; Note.toggle();" width="850" height="1285"/>');
                expect(res.imageUrl).toEqual("https://safebooru.org//images/1112/9dae0eb8ba883f22bc52aafd09dd27ce88e97bfe.png?7303945");
            });

            it("still reads the picture of the old page layout", () => {
                const res = parse('<img alt="x" src="https://example.com/images/1/abc.jpg" id="image" onclick="Note.toggle();" width="1" height="1"/>');
                expect(res.imageUrl).toEqual("https://example.com/images/1/abc.jpg");
            });

            it("reads tag types when the class carries more than the type", () => {
                const res = parse('<li class="tag-type-character tag"><a href="index.php?page=wiki&amp;s=list&amp;search=decidueye">?</a> <a href="index.php?page=post&amp;s=list&amp;tags=decidueye">decidueye</a> <span class="tag-count">1234</span></li>'
                    + '<li class="tag-type-general"><a href="a">?</a> <a href="b">sky</a> <span style="color: #a0a0a0;">99</span></li>');
                expect(res.tags).toEqual([
                    { name: "decidueye", count: 1234, type: "character" },
                    { name: "sky", count: 99, type: "general" },
                ]);
            });
        });
    });
});
