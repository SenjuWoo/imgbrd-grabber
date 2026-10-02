import { makeGrabber } from "../test-utils";
import { source } from "./model";

describe("Pixiv source errors", () => {
    beforeAll(makeGrabber);
    const api = source.apis.json;
    it("reports server errors across search, gallery, and details", () => {
        const response = JSON.stringify({error: {message: "OAuth token expired"}});
        expect(api.search.parse(response, 200)).toEqual({error: "OAuth token expired"});
        expect(api.gallery!.parse(response, 200)).toEqual({error: "OAuth token expired"});
        expect(api.details!.parse(response, 200)).toEqual({error: "OAuth token expired"});
        expect(api.search.parse("{}", 200)).toHaveProperty("error");
        expect(api.gallery!.parse("{}", 200)).toHaveProperty("error");
        expect(api.details!.parse("{}", 200)).toHaveProperty("error");
    });
    it("retains authenticated search URLs and ordinary search responses", () => {
        expect(api.search.url({search: "landscape", page: 1}, {baseUrl: "https://www.pixiv.net", loggedIn: false, page: 1, limit: 30}, undefined)).toHaveProperty("error");
        const url = api.search.url({search: "landscape", page: 1}, {baseUrl: "https://www.pixiv.net", loggedIn: true, page: 1, limit: 30}, undefined);
        expect(url).toContain("word=landscape");
        const response = {illusts: [{id: 10, title: "Landscape", image_urls: {large: "https://i.pximg.net/image.jpg", medium: "https://i.pximg.net/medium.jpg", small: "https://i.pximg.net/small.jpg"}, tags: [], user: {id: 20, name: "Artist"}}], next_url: "https://app-api.pixiv.net/next"};
        const result = api.search.parse(JSON.stringify(response), 200) as IParsedSearch;
        expect(result.images[0].id).toBe(10);
        expect(result.urlNextPage).toBe(response.next_url);
    });
});
