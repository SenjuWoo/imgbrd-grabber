const tagTypeMap: Record<string, string> = {
    "tag": "general",
    "metadata": "meta",
};

function isMd5(value: any): boolean {
    return typeof value === "string" && /^[0-9a-f]{32}$/i.test(value);
}

// The original file of a post page: its "Original image" link, then the page's og:image, then the picture
// shown (a sample on large posts). The picture's attributes changed order and gained a Cloudflare prefix
// in its onclick over the years, so none of that is assumed.
function originalImageUrl(src: string): string | undefined {
    const patterns = [
        /<a[^>]+href="([^"]+)"[^>]*>\s*Original image\s*<\/a>/i,
        /<meta[^>]+property="og:image"[^>]+content="([^"]+)"/i,
        /<meta[^>]+content="([^"]+)"[^>]+property="og:image"/i,
        /<img[^>]+src="([^"]+)"[^>]+id="image"/i,
        /<img[^>]+id="image"[^>]+src="([^"]+)"/i,
        /<img[^>]+src="([^"]+)"[^>]+onclick="[^"]*Note\.toggle\(\);"/i,
    ];
    for (const pattern of patterns) {
        const match = src.match(pattern);
        if (match && match[1].length > 4) {
            return match[1].replace(/&amp;/g, "&");
        }
    }
    return undefined;
}

// api.rule34.xxx (and gelbooru.com) refuse anonymous API calls. The app recognises the "Missing
// authentication" wording, skips the API for the session and uses the site's HTML pages instead.
function withAuthHint(message: string): string {
    if (/^Missing authentication/i.test(message)) {
        return message + ". Enter your API user ID and key (rule34.xxx > Account > Options) in Options > Sources > this site > Login, type URL.";
    }
    return message;
}

// A JSON reply, or the site's error message. Errors come as a bare JSON string (HTTP 200) or as
// {"success": false, "message": ...}.
function parseJsonReply(src: string): { data?: any; error?: string } {
    let parsed: any;
    try {
        parsed = JSON.parse(src);
    } catch (e: any) {
        return { error: "Invalid JSON response" };
    }
    if (typeof parsed === "string") {
        return { error: withAuthHint(parsed) };
    }
    if (!parsed || typeof parsed !== "object") {
        return { error: "Invalid JSON response" };
    }
    if (!Array.isArray(parsed) && (parsed["success"] === false || parsed["success"] === "false")) {
        return { error: withAuthHint(String(parsed["message"] || parsed["reason"] || "Unknown API error")) };
    }
    return { data: parsed };
}

function completeImage(img: IImage): IImage {
    img.author = (img as any).owner;

    if ("tag_info" in img) {
        const tagInfo: { tag: string; type: string; count: number }[] = img["tag_info"] as any;
        img.tags = tagInfo.map((tag) => ({
            name: tag.tag,
            type: tagTypeMap[tag.type] || tag.type,
            count: tag.count,
        }));
    }

    // The JSON API calls the checksum "hash". File names are not checksums everywhere: safebooru.org and
    // rule34.xxx name files with a 40-character hash, which must not be taken for the MD5 (it breaks
    // duplicate detection, merging across sources and %md5% file names). Without one, the saved file is hashed.
    const hash = (img as any).hash;
    if (!isMd5(img.md5) && isMd5(hash)) {
        img.md5 = hash;
    }
    if (!isMd5(img.md5)) {
        delete img.md5;
    }

    if ((!img.file_url || img.file_url.length < 5) && img.preview_url) {
        img.file_url = img.preview_url
            .replace("/thumbnails/", "/images/")
            .replace("/thumbnail_", "/");
    }

    return img;
}

export const source: ISource = {
    name: "Gelbooru (0.2)",
    modifiers: [
        "rating:general", "rating:safe", "rating:questionable", "rating:explicit",
        "user:", "fav:", "fastfav:", "md5:", "source:", "id:", "width:", "height:", "score:", "mpixels:", "filesize:", "date:",
        "gentags:", "arttags:", "chartags:", "copytags:", "approver:", "parent:", "sub:", "parent:none", "unlocked:rating",
        // freestyle aliases (normalized to sort:field:dir)
        "order:id", "order:id_desc", "order:id_asc", "order:score", "order:score_asc", "order:score_desc",
        "order:newest", "order:recent", "order:descending", "order:desc", "order:ascending", "order:asc", "order:oldest",
        "order:date", "order:updated", "order:popular", "order:rank", "order:random",
        "order:mpixels", "order:mpixels_asc", "order:filesize", "order:landscape", "order:portrait", "order:favcount",
        // native Gelbooru form
        "sort:id:desc", "sort:id:asc", "sort:score:desc", "sort:score:asc", "sort:updated:desc", "sort:updated:asc",
        "sort:rating:desc", "sort:user:desc", "sort:height:desc", "sort:width:desc", "sort:parent:desc", "sort:source:desc",
        "sort:random", "sort:updated", "sort:id", "sort:score", "sort:rating", "sort:user", "sort:height", "sort:width", "sort:parent", "sort:source",
    ],
    tagFormat: {
        case: "lower",
        wordSeparator: "_",
    },
    searchFormat: {
        and: " ",
    },
    auth: {
        url:  {
            type: "url",
            fields: [
                {
                    id: "userId",
                    key: "user_id",
                },
                {
                    id: "apiKey",
                    key: "api_key",
                },
            ],
        },
        session: {
            type: "post",
            url: "/index.php?page=account&s=login&code=00",
            fields: [
                {
                    id: "pseudo",
                    key: "user",
                },
                {
                    id: "password",
                    key: "pass",
                    type: "password",
                },
            ],
            check: {
                type: "cookie",
                key: "user_id",
            },
        },
    },
    apis: {
        xml: {
            name: "XML",
            auth: [],
            maxLimit: 100,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions): string | IError => {
                    const page: number = query.page - 1;
                    // BUGFIX: old code did order: → sort: so order:id_desc became sort:id_desc
                    // Gelbooru needs sort:id:desc (colon between field and direction).
                    const search: string = Grabber.normalizeOrderTags(query.search, "gelbooru");
                    const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                    if (fav) {
                        return { error: "XML API cannot search favorites" };
                    }
                    return "/index.php?page=dapi&s=post&q=index&limit=" + opts.limit + "&pid=" + page + "&tags=" + encodeURIComponent(search);
                },
                parse: (src: string): IParsedSearch | IError => {
                    const parsed = Grabber.parseXML(src);
                    if (!parsed) {
                        return { error: "Invalid XML response" };
                    }

                    // Handle error messages
                    if (parsed?.["response"]?.["@attributes"]?.["success"] === "false") {
                        return { error: parsed["response"]["@attributes"]["reason"] };
                    }

                    // Some sites (e.g. api.rule34.xxx) return an <error> document, notably when unauthenticated
                    if ("error" in parsed) {
                        const error = parsed["error"];
                        const message = typeof error === "string" ? error : String(error["#text"] || "Unknown API error");
                        return { error: withAuthHint(message) };
                    }

                    // Validate the shape of the response
                    if (!parsed?.["posts"]) {
                        return { error: "Invalid XML response (no posts found)" };
                    }

                    const data = Grabber.makeArray(parsed.posts.post);
                    const images: IImage[] = [];
                    for (const image of data) {
                        if (image && "id" in image) {
                            images.push(completeImage(Grabber.typedXML(image)));
                        } else if (image && "@attributes" in image) {
                            images.push(completeImage(image["@attributes"]));
                        }
                    }

                    return {
                        images,
                        imageCount: parsed.posts["@attributes"]?.["count"],
                    };
                },
            },
        },
        json: {
            name: "JSON",
            auth: [],
            maxLimit: 100,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions): string | IError => {
                    const page: number = query.page - 1;
                    const search: string = query.search.replace(/(^| )order:/gi, "$1sort:");
                    const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                    if (fav) {
                        return { error: "JSON API cannot search favorites" };
                    }
                    return "/index.php?page=dapi&s=post&q=index&limit=" + opts.limit + "&pid=" + page + "&tags=" + encodeURIComponent(search) + "&json=1";
                },
                parse: (src: string): IParsedSearch | IError => {
                    // A search without results is answered with an empty body
                    if (src.trim().length === 0) {
                        return { images: [] };
                    }

                    const reply = parseJsonReply(src);
                    if (reply.error !== undefined) {
                        return { error: reply.error };
                    }
                    let parsed: any = reply.data;

                    // Handle the old format
                    if (Array.isArray(parsed)) {
                        parsed = {
                            "@attributes": {},
                            "post": parsed,
                        };
                    }

                    const images: IImage[] = [];
                    for (const image of Grabber.makeArray(parsed["post"])) {
                        if (image) {
                            images.push(completeImage(image));
                        }
                    }

                    return {
                        images,
                        imageCount: parsed["@attributes"]?.["count"],
                    };
                },
            },
            details: {
                url: (id: string, md5: string, opts: IUrlDetailsOptions): string => {
                    return "/index.php?page=dapi&s=post&q=index&id=" + id + "&fields=tag_info&json=1";
                },
                parse: (src: string): IParsedDetails | IError => {
                    if (src.trim().length === 0) {
                        return { error: "Post not found" };
                    }
                    const reply = parseJsonReply(src);
                    if (reply.error !== undefined) {
                        return { error: reply.error };
                    }
                    const parsed = reply.data;
                    // A list, the wrapped format ({"@attributes", "post"}; no "post" when the ID is unknown) or the bare post
                    const post = Array.isArray(parsed) ? parsed : "post" in parsed ? parsed["post"] : "@attributes" in parsed ? undefined : parsed;
                    const raw = Array.isArray(post) ? post[0] : post;
                    if (!raw || typeof raw !== "object") {
                        return { error: "Post not found" };
                    }

                    // The real file address is what a download falls back to after a guessed one fails; a
                    // file_url guessed from the thumbnail (completeImage) would only repeat that guess.
                    const fileUrl: string | undefined = typeof raw.file_url === "string" && raw.file_url.length > 4 ? raw.file_url : undefined;
                    const image = completeImage(raw);
                    // Without tag_info the JSON API sends the tags as one space-separated string
                    const rawTags: any = image.tags;
                    const tags: string[] | ITag[] | undefined = Array.isArray(rawTags)
                        ? rawTags
                        : typeof rawTags === "string" ? rawTags.split(" ").filter((tag: string) => tag.length > 0) : undefined;
                    return {
                        tags,
                        imageUrl: fileUrl,
                        createdAt: image.created_at,
                        source: image.source || undefined,
                    };
                },
            },
        },
        html: {
            name: "Regex",
            auth: [],
            forcedLimit: 42,
            search: {
                url: (query: ISearchQuery, opts: IUrlOptions, previous: IPreviousSearch | undefined): string | IError => {
                    try {
                        const baseUrl = opts.baseUrl.replace("//api.", "//");
                        const search: string = Grabber.normalizeOrderTags(query.search, "gelbooru");
                        const fav = search.match(/(?:^| )fav:(\d+)(?:$| )/);
                        if (fav) {
                            const pagePart = Grabber.pageUrl(query.page, previous, 20000, "&pid={page}", "&pid={page}", " id:<{min}&p=1", (p: number) => (p - 1) * 50);
                            return baseUrl + "/index.php?page=favorites&s=view&id=" + fav[1] + pagePart;
                        } else {
                            const pagePart = Grabber.pageUrl(query.page, previous, 20000, "&pid={page}", "&pid={page}", " id:<{min}&p=1", (p: number) => (p - 1) * 42);
                            return baseUrl + "/index.php?page=post&s=list&tags=" + encodeURIComponent(search) + pagePart;
                        }
                    } catch (e: any) {
                        return { error: e.message };
                    }
                },
                parse: (src: string): IParsedSearch | IError => {
                    if (src.indexOf("Unable to search this deep") !== -1) {
                        return { error: "Page too far" };
                    }
                    const pageCountRaw = Grabber.regexMatch('<a href="[^"]+pid=(?<page>\\d+)[^"]*"[^>]*>[^<]+</a>\\s*(?:<b>(?<page_2>\\d+)</b>\\s*)?(?:</div>|<br ?/>)|<a href="[^"]+pid=(?<page_3>\\d+)[^"]*"[^>]*>(?:&raquo;|>>)</a>', src);
                    const pageCount = pageCountRaw && pageCountRaw["page"];
                    const images = Grabber.regexToImages('(?:<span[^>]*(?:\\sid="?\\w(?<id>\\d+)"?)?>\\s*)?<a[^>]*(?:\\sid="?\\w(?<id_2>\\d+)"?)[^>]*>\\s*<img [^>]*(?:src|data-original)="(?<preview_url>[^"]+/thumbnail_(?<md5>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags>[^"]+)"[^>]*/?>\\s*</a>|<img\\s+class="preview"\\s+src="(?<preview_url_2>[^"]+/thumbnail_(?<md5_2>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags_2>[^"]+)"[^>]*/?>', src);
                    for (const img of images) {
                        const json = src.match(new RegExp("posts\\[" + img.id + "\\]\\s*=\\s*({[\\s*\\S]+?})"))?.[1];
                        if (json) {
                            img.rating = json.match(/["']?rating["']?\s*:\s*["']([^"']+)["']/)?.[1].toLowerCase();
                            img.score  = json.match(/["']?score["']?\s*:\s*["'](\d+)["']/)?.[1];
                            img.author = json.match(/["']?user["']?\s*:\s*["']([^'"]+)["']/)?.[1];
                        }
                    }
                    return {
                        images: images.map(completeImage),
                        tags: Grabber.regexToTags('<li class="tag-type-(?<type>[^" ]+)[^"]*">(?:[^<]*<a[^>]*>[^<]*</a>)*[^<]*<a[^>]*>(?<name>[^<]*)</a>[^<]*<span[^>]*>\\s*(?<count>\\d+)\\s*</span>', src),
                        pageCount: pageCount ? parseInt(pageCount, 10) / 42 + 1 : undefined,
                    };
                },
            },
            details: {
                url: (id: string, md5: string, opts: IUrlDetailsOptions): string => {
                    const baseUrl = opts.baseUrl.replace("//api.", "//");
                    return baseUrl + "/index.php?page=post&s=view&id=" + id;
                },
                parse: (src: string): IParsedDetails => {
                    return {
                        tags: Grabber.regexToTags('<li class="tag-type-(?<type>[^" ]+)[^"]*">(?:[^<]*(?:<span[^>]*>[^<]*)?<a[^>]*>[^<]*</a>(?:[^<]*</span>)?)*[^<]*<a[^>]*>(?<name>[^<]*)</a>[^<]*<span[^>]*>(?<count>\\d+)</span>[^<]*</li>', src),
                        imageUrl: originalImageUrl(src),
                        createdAt: src.match(/Posted:\s*([^<]+)</i)?.[1]?.trim(),
                        source: src.match(/>Source:\s*<a href="([^"]+)"/i)?.[1],
                    };
                },
            },
            endpoints: {
                pool_list: {
                    name: "Pools",
                    input: {},
                    url: (query: Record<never, string>, opts: IUrlOptions): string => {
                        const pid = (opts.page - 1) * 25;
                        return "/index.php?page=pool&s=list&pid=" + String(pid);
                    },
                    parse: (src: string): IParsedSearch => {
                        const html = Grabber.parseHTML(src);

                        const images: IImage[] = [];

                        const rows = html.find("table tr");
                        for (const row of rows) {
                            const parts = row.find("td");
                            const link = parts[1].find("a")[0];
                            const id = link.attr("href").match(/id=(\d+)/)[1];

                            images.push({
                                id,
                                name: link.innerText(),
                                type: "gallery",
                                gallery_count: parts[2].innerText().match(/(\d+)\s+Images/)[1],
                                details_endpoint: {
                                    endpoint: "pool_details",
                                    input: { id },
                                },
                            })
                        }

                        return { images };
                    },
                },
                pool_details: {
                    input: {
                        id: {
                            type: "input",
                        },
                    },
                    url: (query: Record<"id", number>): string => {
                        return "/index.php?page=pool&s=show&id=" + String(query.id);
                    },
                    parse: (src: string): IParsedGallery => {
                        const images = Grabber.regexToImages('<span[^>]*(?: id="?\\w(?<id>\\d+)"?)?>\\s*<a[^>]*(?: id="?\\w(?<id_2>\\d+)"?)[^>]*>\\s*<img [^>]*(?:src|data-original)="(?<preview_url>[^"]+/thumbnail_(?<md5>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags>[^"]+)"[^>]*/?>\\s*</a>|<img\\s+class="preview"\\s+src="(?<preview_url_2>[^"]+/thumbnail_(?<md5_2>[^.]+)\\.[^"]+)" [^>]*title="\\s*(?<tags_2>[^"]+)"[^>]*/?>', src);
                        return {
                            images: images.map(completeImage),
                        };
                    },
                },
            },
            tagTypes: false,
            tags: {
                url: (query: ITagsQuery, opts: IUrlOptions): string => {
                    const baseUrl = opts.baseUrl.replace("//api.", "//");
                    const sorts = { count: "desc", date: "asc", name: "asc" };
                    const orderBys = { count: "index_count", date: "updated", name: "tag" };
                    const page: number = (query.page - 1) * 50;
                    return baseUrl + "/index.php?page=tags&s=list&pid=" + page + "&sort=" + sorts[query.order] + "&order_by=" + orderBys[query.order];
                },
                parse: (src: string): IParsedTags => {
                    return {
                        tags: Grabber.regexToTags('(?:<td>(?<count_2>\\d+)</td>\\s*)?<td><span class="tag-type-(?<type>[^"]+)"><a[^>]+>(?<name>.+?)</a></span>(?:\\s*<span class="tag-count">(?<count>\\d+)</span>)?</td>', src),
                    };
                },
            },
            check: {
                url: (): string => {
                    return "/";
                },
                parse: (src: string): boolean => {
                    return src.search(/Running Gelbooru(?: Beta)? 0\.2/) !== -1
                        || src.search(/Running <a[^>]*>Gelbooru<\/a>(?: Beta)? 0\.2/) !== -1;
                },
            },
        },
    },
};
