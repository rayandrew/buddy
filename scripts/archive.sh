git archive -o buddy.tar --prefix buddy/ @
git -C apps/qs archive -o ../../qs.tar --prefix buddy/apps/qs/ @
tar --concatenate -f buddy.tar qs.tar
rm qs.tar
gzip buddy.tar
