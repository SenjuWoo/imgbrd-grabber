import { makeGrabber, search } from "../test-utils";
import { source } from "./model";

describe("Reddit source", () => {
    beforeAll(makeGrabber);
    const api = source.apis.json;
    const post = {kind: "t3", data: {id: "abc123", title: "Forest", url: "https://i.redd.it/forest.png", post_hint: "image", subreddit: "EarthPorn"}};

    it("preserves keyword filters and uses listing endpoints for empty searches", () => {
        expect(search(api, "subreddit:EarthPorn sort:new")).toContain("/r/EarthPorn/new.json?");
        const url = String(search(api, "author:artist blue_sky rating:safe"));
        expect(url).toMatch(/^\/search.json\?/);
        expect(decodeURIComponent(url)).toContain("blue sky author:artist self:no");
        expect(url).not.toContain("rating");
        expect(search(api, "cat", 3)).toHaveProperty("error");
    });

    it("uses the returned cursor without dropping or duplicating the query", () => {
        const url = "https://www.reddit.com/search.json?q=forest&limit=10&after=t3_old";
        const result = api.search.parse(JSON.stringify({kind: "Listing", data: {children: [post], after: "t3_next"}}), 200, url) as IParsedSearch;
        expect(result.images).toHaveLength(1);
        expect(result.urlNextPage).toBe("https://www.reddit.com/search.json?q=forest&limit=10&after=t3_next");
        expect(result.imageCount).toBeUndefined(); // Returned media count is not a total.
        const last = api.search.parse(JSON.stringify({kind: "Listing", data: {children: [], after: null}}), 200, url) as IParsedSearch;
        expect(last.urlNextPage).toBeUndefined();
    });

    it("reports denied access and malformed listings and extracts crosspost media", () => {
        expect(api.search.parse("<html>Denied</html>", 403)).toHaveProperty("error");
        expect(api.search.parse('{"kind":"Listing","data":{}}', 200)).toHaveProperty("error");
        const child = {kind: "t3", data: {url: "https://www.reddit.com/r/demo/comments/id", crosspost_parent_list: [post.data]}};
        const result = api.search.parse(JSON.stringify({kind: "Listing", data: {children: [child]}}), 200) as IParsedSearch;
        expect(result.images[0].file_url).toBe(post.data.url);
    });
});
